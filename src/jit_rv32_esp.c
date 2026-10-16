#include "jit_rv32_esp.h"
#include "i386_struct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BUILD_ESP32
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#endif

/* --- RV32 Registers --- */
#define REG_ZERO 0
#define REG_RA   1
#define REG_SP   2
#define REG_GP   3
#define REG_TP   4
#define REG_T0   5
#define REG_T1   6
#define REG_T2   7
#define REG_S0   8  /* Host pointer to CPUI386 */
#define REG_S1   9  /* x86 EAX */
#define REG_A0   10
#define REG_A1   11
#define REG_A2   12
#define REG_A3   13
#define REG_A4   14
#define REG_A5   15
#define REG_A6   16
#define REG_A7   17
#define REG_S2   18 /* x86 ECX */
#define REG_S3   19 /* x86 EDX */
#define REG_S4   20 /* x86 EBX */
#define REG_S5   21 /* x86 ESP */
#define REG_S6   22 /* x86 EBP */
#define REG_S7   23 /* x86 ESI */
#define REG_S8   24 /* x86 EDI */
#define REG_S9   25
#define REG_S10  26
#define REG_S11  27
#define REG_T3   28
#define REG_T4   29
#define REG_T5   30
#define REG_T6   31

/* Map x86 32-bit GPR index (0..7: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI) to RV32 register */
static const int x86_to_rv[8] = {
    REG_S1, /* 0: EAX */
    REG_S2, /* 1: ECX */
    REG_S3, /* 2: EDX */
    REG_S4, /* 3: EBX */
    REG_S5, /* 4: ESP */
    REG_S6, /* 5: EBP */
    REG_S7, /* 6: ESI */
    REG_S8  /* 7: EDI */
};

/* ESP32-P4: Hybrid Memory Topology
 * - JIT Code Buffer (4MB): Allocated in external PSRAM (cached by ESP32-P4 32KB L1 I-cache).
 * - JIT Hash Table (8192 sets x 2 ways = 192KB): Kept in zero-wait-state Internal HP L2 SRAM.
 * - Hotspot Table (8KB) & LRU Table (8KB): Kept in Internal HP L2 SRAM.
 * This guarantees zero JIT cache thrashing for Windows 95 while keeping lookup ultra-fast!
 */
#ifdef BUILD_ESP32
#define JIT_BUF_SIZE        (4 * 1024 * 1024)   /* 4MB executable buffer in PSRAM */
#define JIT_HASH_SETS       8192                /* 8K sets (192KB in internal HP L2 SRAM) */
#else
/* Host build (e.g. qemu-riscv32): RAM is plentiful, so keep the whole hot set
   resident. A small cache would constantly flush and recompile Windows 95's
   working set, which is far slower than having a larger buffer. */
#define JIT_BUF_SIZE        (16 * 1024 * 1024)  /* 16MB executable buffer */
#define JIT_HASH_SETS       16384               /* 16K sets */
#endif
#define JIT_HASH_WAYS       2               /* 2-way set associative */
#define JIT_HOTSPOT_SIZE    8192            /* 8K tag-validated hotspot entries (32KB tags + 8KB counts) */
#define JIT_HOTSPOT_THRESHOLD 64            /* Trigger compile on 64 executions of the exact PC.
                                               Compilation is expensive on the ESP32 and the code
                                               pool is small, so only compile genuinely hot code. */

typedef struct {
    uint32_t guest_pc;
    uint32_t phys_and_insns; /* high 20 bits: physical page number of the code page; low 12 bits: insn_count (always >= 1) */
    uint32_t code_off;       /* byte offset of the block within s_jit_buf */
} JITEntry;

static uint8_t *s_jit_buf = NULL;
static size_t s_jit_off = 0;
static JITEntry (*s_hash_tab)[JIT_HASH_WAYS] = NULL;
static uint8_t *s_hash_lru = NULL;
/* Hotspot: direct-mapped, tag-validated per-guest-PC counters (see jit_arm64.c).
 * The exact PC tag stops unrelated PCs aliasing one slot from inflating each
 * other's count, which previously made the compile threshold fire on cold code. */
static uint32_t *s_hotspot_tag = NULL;
static uint8_t *s_hotspot_cnt = NULL;
#define JIT_HOTSPOT_SHIFT 13 /* log2(JIT_HOTSPOT_SIZE) */
static bool s_initialized = false;
static bool s_jit_enabled = true;
static int s_jit_ring0_allowed = 0;
static uint32_t s_jit_exec_count = 0;
static uint32_t s_jit_compile_count = 0;

uint8_t *jit_smc_page_map = NULL;
uint8_t *jit_page_gen = NULL;
static uint8_t *s_entry_gen = NULL; /* [JIT_HASH_SETS][JIT_HASH_WAYS]: page gen at insert */
volatile uint8_t jit_smc_dirty = 0;

/* Chain link sentinel: a successor with no compiled (and still valid) block. */
#define JIT_NO_CHAIN        0xFFFFFFFFu

/* Compiled-block frame layout (64 bytes):
 *   [sp,#0]   cached &s_xlate_epoch   (mapping-epoch stamp; chain guard)
 *   [sp,#4]   cached jit_page_gen base (chain guard)
 *   [sp,#8..47] saved s8..s1, s0, ra  (see emit_prologue)
 *   [sp,#48]  cpu_load32() result scratch (see JIT_SLOT_LOAD)
 *   [sp,#56]  EA spill for read-modify-write memory ops (survives cpu_* calls)
 * Bytes 0/4 hold the two pointers a chain slot needs so each guard costs a
 * single load instead of a 2-instruction address materialise.  The load
 * scratch MUST NOT alias those two slots: cpu_load32 writes its result through
 * the pointer handed to it, and a load/push/pop in the block body would then
 * overwrite the very base pointer the chain tail is about to read, silently
 * disabling chaining for every block that touches memory. */
#define JIT_SLOT_GUARD      0
#define JIT_SLOT_PAGEGEN    4
#define JIT_SLOT_LOAD       48
#define JIT_SLOT_TLB        52
#define JIT_SLOT_EA         56

#define CPU_OFF_INTR   ((int32_t)offsetof(struct CPUI386, intr))
#define CPU_OFF_CYCLE  ((int32_t)offsetof(struct CPUI386, cycle))
/* seg[i].base sits 4 bytes into each 16-byte segment descriptor */
#define CPU_OFF_SEG_BASE(i) ((int32_t)(offsetof(struct CPUI386, seg) + 4 + 16 * (i)))

/* Mapping-epoch stamp. There is no transition cache in this backend, but the
 * chain guards need to notice a VA->PA change (CR3 switch / INVLPG / CR0.PG)
 * so a statically linked successor is re-validated by the dispatcher. Never 0. */
static uint32_t s_xlate_epoch = 1;

static inline uint32_t jit_pack_tag(uint32_t phys, uint32_t insns) {
    return ((phys & 0xFFFFFu) << 12) | (insns & 0xFFFu);
}

static inline void *jit_entry_fn(const JITEntry *e) {
    return (void *)(s_jit_buf + e->code_off);
}

/* External helpers declared in i386.h */
extern bool cpu_load8(CPUI386 *cpu, int seg, uint32_t addr, uint8_t *res);
extern bool cpu_load32(CPUI386 *cpu, int seg, uint32_t addr, uint32_t *res);
extern bool cpu_store32(CPUI386 *cpu, int seg, uint32_t addr, uint32_t val);
extern uint32_t cpu_jit_va_page(CPUI386 *cpu, uint32_t va);

/* Returns the 20-bit physical page key for guest_pc, or *ok = false when the
 * mapping is indeterminate (lookup must then decline to reuse a block). */
static inline uint32_t jit_key_of(CPUI386 *cpu, uint32_t guest_pc, bool *ok) {
    uint32_t phys = cpu_jit_va_page(cpu, guest_pc);
    if (phys == 0xFFFFFFFFu) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return phys & 0xFFFFFu;
}

/* Generation recorded for the physical page holding a compiled block; 0 when
 * the page is not tracked by the SMC map. */
static inline uint8_t jit_page_gen_of(uint32_t key) {
    return (jit_page_gen && key < JIT_SMC_MAP_PAGES) ? jit_page_gen[key] : 0;
}

void jit_mark_code_pages(CPUI386 *cpu, uint32_t start_va, uint32_t end_va) {
    if (!jit_smc_page_map) return;
    for (uint32_t va = start_va & ~0xFFFu; va < end_va; va += 0x1000u) {
        uint32_t phys = cpu_jit_va_page(cpu, va);
        if (phys == 0xFFFFFFFFu) continue;
        if (phys < JIT_SMC_MAP_PAGES) jit_smc_page_map[phys] = 1;
        if (va > 0xFFFFF000u) break; /* guard against wrap */
    }
    /* A page turned into code: retire any resident flat-TLB entry, since a
     * fast-path store would no longer bump this page's SMC generation. */
    jit_fast_tlb_flush();
}

/* Per-page invalidation epoch. This backend has no transition cache, but the
 * chain guards compare this value against the epoch observed at link time, so
 * every VA->PA remap (tlb_clear) retires all static links at once. See
 * jit_arm64.c for the cached-transition counterpart. */
void jit_note_map_change(void) { s_xlate_epoch++; }

/* --- Instruction Emission Helpers --- */
static inline void emit32(uint32_t **buf, uint32_t val) {
    **buf = val;
    (*buf)++;
}

static inline void emit_addi(uint32_t **buf, int rd, int rs1, int32_t imm12) {
    emit32(buf, 0x13 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | ((imm12 & 0xfff) << 20));
}

static inline void emit_add(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_sub(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | (rs2 << 20) | (0x20 << 25));
}

static inline void emit_and(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x7 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_andi(uint32_t **buf, int rd, int rs1, int32_t imm12) {
    emit32(buf, 0x13 | (rd << 7) | (0x7 << 12) | (rs1 << 15) | ((imm12 & 0xfff) << 20));
}

static inline void emit_or(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x6 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_ori(uint32_t **buf, int rd, int rs1, int32_t imm12) {
    emit32(buf, 0x13 | (rd << 7) | (0x6 << 12) | (rs1 << 15) | ((imm12 & 0xfff) << 20));
}

static inline void emit_xor(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x4 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_xori(uint32_t **buf, int rd, int rs1, int32_t imm12) {
    emit32(buf, 0x13 | (rd << 7) | (0x4 << 12) | (rs1 << 15) | ((imm12 & 0xfff) << 20));
}

static inline void emit_lw(uint32_t **buf, int rd, int rs1, int32_t offset12) {
    emit32(buf, 0x03 | (rd << 7) | (0x2 << 12) | (rs1 << 15) | ((offset12 & 0xfff) << 20));
}

static inline void emit_sw(uint32_t **buf, int rs2, int rs1, int32_t offset12) {
    uint32_t imm = (uint32_t)offset12 & 0xfff;
    emit32(buf, 0x23 | ((imm & 0x1f) << 7) | (0x2 << 12) | (rs1 << 15) | (rs2 << 20) | (((imm >> 5) & 0x7f) << 25));
}

static inline void emit_lbu(uint32_t **buf, int rd, int rs1, int32_t offset12) {
    emit32(buf, 0x03 | (rd << 7) | (0x4 << 12) | (rs1 << 15) | ((offset12 & 0xfff) << 20));
}

static inline void emit_jalr(uint32_t **buf, int rd, int rs1, int32_t offset12) {
    emit32(buf, 0x67 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | ((offset12 & 0xfff) << 20));
}

static inline void emit_ret(uint32_t **buf) {
    emit_jalr(buf, REG_ZERO, REG_RA, 0);
}

static inline void emit_jal(uint32_t **buf, int rd, int32_t offset) {
    uint32_t imm = (uint32_t)offset;
    uint32_t imm20 = (imm >> 20) & 1;
    uint32_t imm10_1 = (imm >> 1) & 0x3ff;
    uint32_t imm11 = (imm >> 11) & 1;
    uint32_t imm19_12 = (imm >> 12) & 0xff;
    emit32(buf, 0x6F | (rd << 7) | (imm19_12 << 12) | (imm11 << 20) | (imm10_1 << 21) | (imm20 << 31));
}

/* Branch: B-type */
static inline void emit_branch(uint32_t **buf, int funct3, int rs1, int rs2, int32_t offset) {
    uint32_t imm = (uint32_t)offset;
    uint32_t b12 = (imm >> 12) & 1;
    uint32_t b10_5 = (imm >> 5) & 0x3f;
    uint32_t b4_1 = (imm >> 1) & 0xf;
    uint32_t b11 = (imm >> 11) & 1;
    emit32(buf, 0x63 | (b11 << 7) | (b4_1 << 8) | ((funct3 & 0x7) << 12) | (rs1 << 15) | (rs2 << 20) | (b10_5 << 25) | (b12 << 31));
}

static inline void emit_beq(uint32_t **buf, int rs1, int rs2, int32_t offset) {
    emit_branch(buf, 0x0, rs1, rs2, offset);
}

static inline void emit_bne(uint32_t **buf, int rs1, int rs2, int32_t offset) {
    emit_branch(buf, 0x1, rs1, rs2, offset);
}

static inline void emit_blt(uint32_t **buf, int rs1, int rs2, int32_t offset) {
    emit_branch(buf, 0x4, rs1, rs2, offset);
}

static inline void emit_bge(uint32_t **buf, int rs1, int rs2, int32_t offset) {
    emit_branch(buf, 0x5, rs1, rs2, offset);
}

static inline void emit_bltu(uint32_t **buf, int rs1, int rs2, int32_t offset) {
    emit_branch(buf, 0x6, rs1, rs2, offset);
}

static inline void emit_bgeu(uint32_t **buf, int rs1, int rs2, int32_t offset) {
    emit_branch(buf, 0x7, rs1, rs2, offset);
}

/* LUI: U-type */
static inline void emit_lui(uint32_t **buf, int rd, uint32_t imm20) {
    emit32(buf, 0x37 | (rd << 7) | (imm20 & 0xfffff000));
}

static void emit_mov_imm32(uint32_t **buf, int rd, uint32_t imm32) {
    int32_t simm = (int32_t)imm32;
    if (simm >= -2048 && simm <= 2047) {
        emit_addi(buf, rd, REG_ZERO, simm);
    } else {
        uint32_t hi = (imm32 + 0x800) & 0xfffff000;
        int32_t lo = (int32_t)(imm32 - hi);
        emit_lui(buf, rd, hi);
        if (lo != 0) {
            emit_addi(buf, rd, rd, lo);
        }
    }
}

static inline void emit_sltu(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x3 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_sltiu(uint32_t **buf, int rd, int rs1, int32_t imm12) {
    emit32(buf, 0x13 | (rd << 7) | (0x3 << 12) | (rs1 << 15) | ((imm12 & 0xfff) << 20));
}

static inline void emit_slli(uint32_t **buf, int rd, int rs1, uint32_t shamt5) {
    emit32(buf, 0x13 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | ((shamt5 & 0x1f) << 20) | (0x00 << 25));
}

static inline void emit_srli(uint32_t **buf, int rd, int rs1, uint32_t shamt5) {
    emit32(buf, 0x13 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | ((shamt5 & 0x1f) << 20) | (0x00 << 25));
}

static inline void emit_srai(uint32_t **buf, int rd, int rs1, uint32_t shamt5) {
    emit32(buf, 0x13 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | ((shamt5 & 0x1f) << 20) | (0x20 << 25));
}

static inline void emit_sll(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_srl(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_sra(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | (rs2 << 20) | (0x20 << 25));
}

static inline void emit_mul(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | (rs2 << 20) | (0x01 << 25));
}

static inline void emit_div(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x4 << 12) | (rs1 << 15) | (rs2 << 20) | (0x01 << 25));
}

static inline void emit_divu(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | (rs2 << 20) | (0x01 << 25));
}

static inline void emit_rem(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x6 << 12) | (rs1 << 15) | (rs2 << 20) | (0x01 << 25));
}

static inline void emit_remu(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x7 << 12) | (rs1 << 15) | (rs2 << 20) | (0x01 << 25));
}

/* --- Zba: Address Generation --- */
static inline void emit_sh1add(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x2 << 12) | (rs1 << 15) | (rs2 << 20) | (0x10 << 25));
}

static inline void emit_sh2add(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x4 << 12) | (rs1 << 15) | (rs2 << 20) | (0x10 << 25));
}

static inline void emit_sh3add(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x6 << 12) | (rs1 << 15) | (rs2 << 20) | (0x10 << 25));
}

/* --- Zbb: Basic Bit Manipulation --- */
static inline void emit_rol(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (rs2 << 20) | (0x30 << 25));
}

static inline void emit_ror(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | (rs2 << 20) | (0x30 << 25));
}

static inline void emit_rori(uint32_t **buf, int rd, int rs1, uint32_t shamt) {
    emit32(buf, 0x13 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | ((shamt & 0x1f) << 20) | (0x30 << 25));
}

static inline void emit_clz(uint32_t **buf, int rd, int rs1) {
    emit32(buf, 0x13 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (0x000 << 20) | (0x30 << 25));
}

static inline void emit_ctz(uint32_t **buf, int rd, int rs1) {
    emit32(buf, 0x13 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (0x001 << 20) | (0x30 << 25));
}

static inline void emit_andn(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x7 << 12) | (rs1 << 15) | (rs2 << 20) | (0x20 << 25));
}

/* --- Zbs: Single-bit Operations --- */
static inline void emit_bext(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | (rs2 << 20) | (0x24 << 25));
}

static inline void emit_bexti(uint32_t **buf, int rd, int rs1, uint32_t shamt) {
    emit32(buf, 0x13 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | ((shamt & 0x1f) << 20) | (0x24 << 25));
}

static inline void emit_bset(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (rs2 << 20) | (0x14 << 25));
}

static inline void emit_bseti(uint32_t **buf, int rd, int rs1, uint32_t shamt) {
    emit32(buf, 0x13 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | ((shamt & 0x1f) << 20) | (0x14 << 25));
}

static inline void emit_bclr(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (rs2 << 20) | (0x24 << 25));
}

static inline void emit_bclri(uint32_t **buf, int rd, int rs1, uint32_t shamt) {
    emit32(buf, 0x13 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | ((shamt & 0x1f) << 20) | (0x24 << 25));
}

static inline void emit_binv(uint32_t **buf, int rd, int rs1, int rs2) {
    emit32(buf, 0x33 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | (rs2 << 20) | (0x34 << 25));
}

static inline void emit_binvi(uint32_t **buf, int rd, int rs1, uint32_t shamt) {
    emit32(buf, 0x13 | (rd << 7) | (0x1 << 12) | (rs1 << 15) | ((shamt & 0x1f) << 20) | (0x34 << 25));
}

static inline void emit_rev8(uint32_t **buf, int rd, int rs1) {
    emit32(buf, 0x13 | (rd << 7) | (0x5 << 12) | (rs1 << 15) | (0x698 << 20));
}

static void emit_flags_cf(uint32_t **buf, int bit_reg) {
    /* bit_reg bit 0 contains the new CF */
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_andi(buf, REG_T4, REG_T4, -2); /* clear CF (bit 0) */
    emit_andi(buf, REG_T0, bit_reg, 1);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

static void emit_flags_zf(uint32_t **buf, int is_zero_reg) {
    /* is_zero_reg: 1 if zero, 0 if non-zero */
    emit_slli(buf, REG_T0, is_zero_reg, 6); /* ZF is bit 6 */
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_bclri(buf, REG_T4, REG_T4, 6);     /* Zbs bclri to clear ZF */
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

static void emit_write_r8(uint32_t **buf, int rm, int val_reg) {
    /* rm 0..3: AL, CL, DL, BL (low byte of EAX, ECX, EDX, EBX)
       rm 4..7: AH, CH, DH, BH (byte 1 of EAX, ECX, EDX, EBX) */
    int target_reg = (rm < 4) ? x86_to_rv[rm] : x86_to_rv[rm - 4];
    emit_andi(buf, REG_T0, val_reg, 0xFF);
    if (rm < 4) {
        emit_srli(buf, REG_T1, target_reg, 8);
        emit_slli(buf, REG_T1, REG_T1, 8);
        emit_or(buf, target_reg, REG_T1, REG_T0);
    } else {
        emit_slli(buf, REG_T0, REG_T0, 8);
        emit_mov_imm32(buf, REG_T2, 0xFFFF00FF);
        emit_and(buf, REG_T1, target_reg, REG_T2);
        emit_or(buf, target_reg, REG_T1, REG_T0);
    }
}

/*
 * EFLAGS Synchronizers:
 * Writes updated CF, ZF, SF, OF directly into cpu->flags (offset 40)
 * and clears cpu->cc.mask (offset 288) so the C interpreter reads cpu->flags directly.
 *
 * Uses scratch registers REG_T0, REG_T2, REG_T3, REG_T4.
 * DOES NOT CLOBBER REG_T1 (x6) or REG_S0..S8!
 */
static void emit_flags_logic(uint32_t **buf, int res_reg) {
    /* CF = 0, OF = 0 */
    /* ZF = (res == 0) */
    emit_sltiu(buf, REG_T2, res_reg, 1);
    emit_slli(buf, REG_T2, REG_T2, 6); /* ZF at bit 6 */
    /* SF = (res < 0) */
    emit_srli(buf, REG_T0, res_reg, 31);
    emit_slli(buf, REG_T0, REG_T0, 7); /* SF at bit 7 */
    emit_or(buf, REG_T0, REG_T0, REG_T2);

    /* Merge into cpu->flags: clear CF(0), ZF(6), SF(7), OF(11) = ~0x8C1 = 0xFFFFF73E */
    emit_mov_imm32(buf, REG_T3, 0xFFFFF73E);
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_and(buf, REG_T4, REG_T4, REG_T3);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

static void emit_flags_sub(uint32_t **buf, int res_reg, int r1_reg, int r2_reg) {
    /* CF = (r1 < r2) unsigned */
    emit_sltu(buf, REG_T0, r1_reg, r2_reg); /* CF at bit 0 */
    /* ZF = (res == 0) */
    emit_sltiu(buf, REG_T2, res_reg, 1);
    emit_slli(buf, REG_T2, REG_T2, 6);
    emit_or(buf, REG_T0, REG_T0, REG_T2);
    /* SF = (res < 0) */
    emit_srli(buf, REG_T2, res_reg, 31);
    emit_slli(buf, REG_T2, REG_T2, 7);
    emit_or(buf, REG_T0, REG_T0, REG_T2);
    /* OF = ((r1 ^ r2) & (r1 ^ res)) >> 31 */
    emit_xor(buf, REG_T2, r1_reg, r2_reg);
    emit_xor(buf, REG_T3, r1_reg, res_reg);
    emit_and(buf, REG_T2, REG_T2, REG_T3);
    emit_srli(buf, REG_T2, REG_T2, 31);
    emit_slli(buf, REG_T2, REG_T2, 11);
    emit_or(buf, REG_T0, REG_T0, REG_T2);

    /* Merge into cpu->flags */
    emit_mov_imm32(buf, REG_T3, 0xFFFFF73E);
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_and(buf, REG_T4, REG_T4, REG_T3);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

static void emit_flags_add(uint32_t **buf, int res_reg, int r1_reg, int r2_reg) {
    /* CF = (res < r1) unsigned */
    emit_sltu(buf, REG_T0, res_reg, r1_reg); /* CF at bit 0 */
    /* ZF = (res == 0) */
    emit_sltiu(buf, REG_T2, res_reg, 1);
    emit_slli(buf, REG_T2, REG_T2, 6);
    emit_or(buf, REG_T0, REG_T0, REG_T2);
    /* SF = (res < 0) */
    emit_srli(buf, REG_T2, res_reg, 31);
    emit_slli(buf, REG_T2, REG_T2, 7);
    emit_or(buf, REG_T0, REG_T0, REG_T2);
    /* OF = ((r1 ^ ~r2) & (r1 ^ res)) >> 31 */
    emit_xor(buf, REG_T2, r1_reg, r2_reg);
    emit_xori(buf, REG_T2, REG_T2, -1);
    emit_xor(buf, REG_T3, r1_reg, res_reg);
    emit_and(buf, REG_T2, REG_T2, REG_T3);
    emit_srli(buf, REG_T2, REG_T2, 31);
    emit_slli(buf, REG_T2, REG_T2, 11);
    emit_or(buf, REG_T0, REG_T0, REG_T2);

    /* Merge into cpu->flags */
    emit_mov_imm32(buf, REG_T3, 0xFFFFF73E);
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_and(buf, REG_T4, REG_T4, REG_T3);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

static void emit_flags_inc(uint32_t **buf, int res_reg) {
    /* CF is untouched in INC! */
    /* OF = (res == 0x80000000) */
    emit_mov_imm32(buf, REG_T3, 0x80000000);
    emit_xor(buf, REG_T2, res_reg, REG_T3);
    emit_sltiu(buf, REG_T2, REG_T2, 1);
    emit_slli(buf, REG_T0, REG_T2, 11);
    /* ZF = (res == 0) */
    emit_sltiu(buf, REG_T2, res_reg, 1);
    emit_slli(buf, REG_T2, REG_T2, 6);
    emit_or(buf, REG_T0, REG_T0, REG_T2);
    /* SF = (res < 0) */
    emit_srli(buf, REG_T2, res_reg, 31);
    emit_slli(buf, REG_T2, REG_T2, 7);
    emit_or(buf, REG_T0, REG_T0, REG_T2);

    /* Clear ZF(6), SF(7), OF(11) = ~0x8C0 = 0xFFFFF73F (CF is preserved) */
    emit_mov_imm32(buf, REG_T3, 0xFFFFF73F);
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_and(buf, REG_T4, REG_T4, REG_T3);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

static void emit_flags_dec(uint32_t **buf, int res_reg) {
    /* CF is untouched in DEC! */
    /* OF = (res == 0x7FFFFFFF) */
    emit_mov_imm32(buf, REG_T3, 0x7FFFFFFF);
    emit_xor(buf, REG_T2, res_reg, REG_T3);
    emit_sltiu(buf, REG_T2, REG_T2, 1);
    emit_slli(buf, REG_T0, REG_T2, 11);
    /* ZF = (res == 0) */
    emit_sltiu(buf, REG_T2, res_reg, 1);
    emit_slli(buf, REG_T2, REG_T2, 6);
    emit_or(buf, REG_T0, REG_T0, REG_T2);
    /* SF = (res < 0) */
    emit_srli(buf, REG_T2, res_reg, 31);
    emit_slli(buf, REG_T2, REG_T2, 7);
    emit_or(buf, REG_T0, REG_T0, REG_T2);

    /* Clear ZF(6), SF(7), OF(11) = ~0x8C0 = 0xFFFFF73F (CF is preserved) */
    emit_mov_imm32(buf, REG_T3, 0xFFFFF73F);
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_and(buf, REG_T4, REG_T4, REG_T3);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
}

/* SHLD/SHRD (486; register form, mod==3 only). count_reg holds the shift count
 * already clamped to 1..31 (the caller skips the instruction when count==0).
 * res = (x << count) | (y >> (32-count)) for SHLD, mirrored for SHRD, where
 * x = original destination value and y = source register.
 * CF = last bit shifted out of x, OF = MSB(x) ^ MSB(res), ZF/SF from res.
 * AF/PF are deliberately left stale, matching this backend's other flag paths.
 * Clobbers T0,T2,T3,T4,T5; the result is carried in T1 and written last so a
 * dst==src (rm==reg) encoding still leaves the shifted sum in place. */
static void emit_shxd(uint32_t **buf, int dst_rv, int src_rv, int count_reg, int is_shld) {
    /* x = original dst */
    emit_addi(buf, REG_T2, dst_rv, 0);
    /* 32 - count -> REG_T5 */
    emit_addi(buf, REG_T5, REG_ZERO, 32);
    emit_sub(buf, REG_T5, REG_T5, count_reg);
    /* res -> REG_T1 */
    if (is_shld) {
        emit_sll(buf, REG_T1, dst_rv, count_reg);
        emit_srl(buf, REG_T0, src_rv, REG_T5);
    } else {
        emit_srl(buf, REG_T1, dst_rv, count_reg);
        emit_sll(buf, REG_T0, src_rv, REG_T5);
    }
    emit_or(buf, REG_T1, REG_T1, REG_T0);
    /* CF -> REG_T0 bit 0 */
    if (is_shld) {
        emit_srl(buf, REG_T0, REG_T2, REG_T5);
    } else {
        emit_addi(buf, REG_T5, count_reg, -1);
        emit_srl(buf, REG_T0, REG_T2, REG_T5);
    }
    emit_andi(buf, REG_T0, REG_T0, 1);
    /* ZF = (res == 0) at bit 6 */
    emit_sltiu(buf, REG_T3, REG_T1, 1);
    emit_slli(buf, REG_T3, REG_T3, 6);
    emit_or(buf, REG_T0, REG_T0, REG_T3);
    /* SF = (res < 0) at bit 7 */
    emit_srli(buf, REG_T3, REG_T1, 31);
    emit_slli(buf, REG_T3, REG_T3, 7);
    emit_or(buf, REG_T0, REG_T0, REG_T3);
    /* OF = MSB(x) ^ MSB(res) at bit 11 */
    emit_xor(buf, REG_T3, REG_T2, REG_T1);
    emit_srli(buf, REG_T3, REG_T3, 31);
    emit_slli(buf, REG_T3, REG_T3, 11);
    emit_or(buf, REG_T0, REG_T0, REG_T3);
    /* Merge: clear CF(0), ZF(6), SF(7), OF(11) = ~0x8C1 = 0xFFFFF73E */
    emit_mov_imm32(buf, REG_T3, 0xFFFFF73E);
    emit_lw(buf, REG_T4, REG_S0, 40);
    emit_and(buf, REG_T4, REG_T4, REG_T3);
    emit_or(buf, REG_T4, REG_T4, REG_T0);
    emit_sw(buf, REG_T4, REG_S0, 40);
    emit_sw(buf, REG_ZERO, REG_S0, 288);
    /* write res (last, so a==b encodings keep the shifted result) */
    emit_addi(buf, dst_rv, REG_T1, 0);
}

void jit_enable(bool enable) {
    s_jit_enabled = enable;
    jit_fast_tlb_active = enable && jit_fast_tlb != NULL;
}

bool jit_is_enabled(void) {
    return s_jit_enabled;
}

uint32_t jit_get_block_exec_count(void) {
    return s_jit_exec_count;
}

uint32_t jit_get_compile_count(void) {
    return s_jit_compile_count;
}

static inline uint32_t jit_hash(uint32_t pc, uint32_t phys) {
    uint32_t k = pc ^ (phys << 5);
    return (k * 2654435761u) & (JIT_HASH_SETS - 1);
}

uint32_t jit_get_block_insn_count(CPUI386 *cpu, uint32_t guest_pc) {
    if (!s_initialized || !s_hash_tab) return 1;
    bool ok; uint32_t key = jit_key_of(cpu, guest_pc, &ok);
    if (!ok) return 1;
    uint32_t set = jit_hash(guest_pc, key);
    uint8_t gen = jit_page_gen_of(key);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        JITEntry *e = &s_hash_tab[set][way];
        if (e->phys_and_insns != 0 && e->guest_pc == guest_pc && (e->phys_and_insns >> 12) == key &&
            s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
            uint32_t n = e->phys_and_insns & 0xFFFu;
            return (n > 0) ? n : 1;
        }
    }
    return 1;
}

/* Side-effect-free check whether a compiled block already exists for guest_pc.
   Used during compilation to avoid inlining already-translated targets. */
static bool __attribute__((unused)) jit_has_block(CPUI386 *cpu, uint32_t guest_pc) {
    if (!s_initialized || !s_hash_tab) return false;
    bool ok; uint32_t key = jit_key_of(cpu, guest_pc, &ok);
    if (!ok) return false;
    uint32_t set = jit_hash(guest_pc, key);
    uint8_t gen = jit_page_gen_of(key);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        JITEntry *e = &s_hash_tab[set][way];
        if (e->phys_and_insns != 0 && e->guest_pc == guest_pc && (e->phys_and_insns >> 12) == key &&
            s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
            return true;
        }
    }
    return false;
}

static inline void jit_sync_range(void *addr, size_t size) {
#if defined(__riscv)
    asm volatile (".insn i 0x0f, 0x1, x0, x0, 0" ::: "memory");
#endif
#ifdef BUILD_ESP32
    if (addr && size > 0) {
        esp_cache_msync(addr, (size + 127) & ~127, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
#endif
}

void jit_flush_cache(void) {
#if defined(__riscv)
    asm volatile (".insn i 0x0f, 0x1, x0, x0, 0" ::: "memory");
#endif
#ifdef BUILD_ESP32
    if (s_jit_buf && s_jit_off > 0) {
        esp_cache_msync(s_jit_buf, (s_jit_off + 127) & ~127, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
#endif
}

void jit_init(void) {
    if (s_initialized) return;

    {
        const char *env = getenv("TINY386_JIT_RING0");
        if (env && env[0] == '0') s_jit_ring0_allowed = 0;
    }

#ifdef BUILD_ESP32
    /* Allocate 4MB JIT Code Buffer in PSRAM (aligned to 128-byte cache line) */
    s_jit_buf = (uint8_t *)heap_caps_aligned_alloc(128, JIT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_jit_buf) {
        s_jit_buf = (uint8_t *)heap_caps_malloc(JIT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_jit_buf) {
        s_jit_buf = (uint8_t *)heap_caps_malloc(2 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_jit_buf) {
        s_jit_buf = (uint8_t *)malloc(JIT_BUF_SIZE);
    }
    printf("[JIT] Code Buffer allocated at %p (%u KB) [Internal L2 SRAM: %s]\n",
           s_jit_buf, (unsigned)(JIT_BUF_SIZE / 1024), esp_ptr_internal(s_jit_buf) ? "YES" : "NO");

    /* Verify execution permissions and hardware extensions in PSRAM */
    if (s_jit_buf) {
        uint32_t *test_code = (uint32_t *)s_jit_buf;

        /* Probe 1: Basic RV32I addi + ret */
        test_code[0] = 0x00150513; /* addi a0, a0, 1 */
        test_code[1] = 0x00008067; /* ret (jalr x0, ra, 0) */
        jit_sync_range(s_jit_buf, 8);
        typedef int (*test_fn1)(int);
        int ret = ((test_fn1)s_jit_buf)(100);
        printf("[JIT] PSRAM execution self-test passed: 100 + 1 = %d\n", ret);

        /* Probe 2: RV32M hardware divide: div a0, a0, a1 (100 / 7 = 14) */
        test_code[0] = 0x02b54533; /* div a0, a0, a1 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        typedef int (*test_fn2)(int, int);
        int div_ret = ((test_fn2)s_jit_buf)(100, 7);
        printf("[JIT] RV32M hardware div test: 100 / 7 = %d (expected 14) [SUPPORTED: %s]\n",
               div_ret, (div_ret == 14) ? "YES" : "NO");

        /* Probe 3: RV32M hardware remainder: rem a0, a0, a1 (100 % 7 = 2) */
        test_code[0] = 0x02b56533; /* rem a0, a0, a1 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        int rem_ret = ((test_fn2)s_jit_buf)(100, 7);
        printf("[JIT] RV32M hardware rem test: 100 %% 7 = %d (expected 2) [SUPPORTED: %s]\n",
               rem_ret, (rem_ret == 2) ? "YES" : "NO");

        /* Probe 4: RVC 16-bit compressed instruction: c.addi a0, 5 (0x0515), c.jr ra (0x8082) */
        uint16_t *test_c = (uint16_t *)s_jit_buf;
        test_c[0] = 0x0515; /* c.addi a0, 5 */
        test_c[1] = 0x8082; /* c.jr ra */
        jit_sync_range(s_jit_buf, 4);
        int c_ret = ((test_fn1)s_jit_buf)(20);
        printf("[JIT] RVC compressed execution test: 20 + 5 = %d (expected 25) [SUPPORTED: %s]\n",
               c_ret, (c_ret == 25) ? "YES" : "NO");

        /* Probe 5: Zba address generation: sh2add a0, a0, a1 (10 * 4 + 5 = 45) */
        test_code[0] = 0x20b54533; /* sh2add a0, a0, a1 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        int sh2add_ret = ((test_fn2)s_jit_buf)(10, 5);
        printf("[JIT] Zba sh2add test: (10 * 4) + 5 = %d (expected 45) [SUPPORTED: %s]\n",
               sh2add_ret, (sh2add_ret == 45) ? "YES" : "NO");

        /* Probe 6: Zbb bit rotation: rori a0, a0, 4 (0x00000080 -> 0x00000008) */
        test_code[0] = 0x60455513; /* rori a0, a0, 4 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        int rori_ret = ((test_fn1)s_jit_buf)(0x00000080);
        printf("[JIT] Zbb rori test: rori(0x80, 4) = 0x%x (expected 0x8) [SUPPORTED: %s]\n",
               rori_ret, (rori_ret == 0x8) ? "YES" : "NO");

        /* Probe 7: Zbs bit extraction: bexti a0, a0, 3 ((0x08 >> 3) & 1 = 1) */
        test_code[0] = 0x48355513; /* bexti a0, a0, 3 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        int bexti_ret = ((test_fn1)s_jit_buf)(0x08);
        printf("[JIT] Zbs bexti test: bexti(0x8, 3) = %d (expected 1) [SUPPORTED: %s]\n",
               bexti_ret, (bexti_ret == 1) ? "YES" : "NO");

        /* Probe 8: Zbs bit inversion: binvi a0, a0, 5 (0 ^ (1 << 5) = 32) */
        test_code[0] = 0x68551513; /* binvi a0, a0, 5 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        int binvi_ret = ((test_fn1)s_jit_buf)(0);
        printf("[JIT] Zbs binvi test: binvi(0, 5) = %d (expected 32) [SUPPORTED: %s]\n",
               binvi_ret, (binvi_ret == 32) ? "YES" : "NO");

        /* Probe 9: Zbb byte reversal: rev8 a0, a0 (0x12345678 -> 0x78563412) */
        test_code[0] = 0x69855513; /* rev8 a0, a0 */
        test_code[1] = 0x00008067; /* ret */
        jit_sync_range(s_jit_buf, 8);
        uint32_t rev8_ret = (uint32_t)((test_fn1)s_jit_buf)(0x12345678);
        printf("[JIT] Zbb rev8 test: rev8(0x12345678) = 0x%08x (expected 0x78563412) [SUPPORTED: %s]\n",
               (unsigned)rev8_ret, (rev8_ret == 0x78563412) ? "YES" : "NO");

        memset(s_jit_buf, 0, 64);
        jit_sync_range(s_jit_buf, 64);
    }
#else
    register void *a0 asm("a0") = NULL;
    register size_t a1 asm("a1") = JIT_BUF_SIZE;
    register int a2 asm("a2") = 7; /* PROT_READ|PROT_WRITE|PROT_EXEC */
    register int a3 asm("a3") = 0x22; /* MAP_PRIVATE|MAP_ANONYMOUS */
    register int a4 asm("a4") = -1;
    register int32_t a5 asm("a5") = 0;
    register int a7 asm("a7") = 222; /* __NR_mmap */
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
    s_jit_buf = (uint8_t *)a0;
#endif

#ifdef BUILD_ESP32
    if (!s_hash_tab) {
        s_hash_tab = (JITEntry (*)[JIT_HASH_WAYS])heap_caps_calloc(JIT_HASH_SETS * JIT_HASH_WAYS, sizeof(JITEntry), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_hash_tab) s_hash_tab = (JITEntry (*)[JIT_HASH_WAYS])heap_caps_calloc(JIT_HASH_SETS * JIT_HASH_WAYS, sizeof(JITEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hash_tab) s_hash_tab = (JITEntry (*)[JIT_HASH_WAYS])calloc(JIT_HASH_SETS * JIT_HASH_WAYS, sizeof(JITEntry));
        printf("[JIT] Hash Table (%u KB) allocated at %p [Internal L2 SRAM: %s]\n",
               (unsigned)((JIT_HASH_SETS * JIT_HASH_WAYS * sizeof(JITEntry)) / 1024),
               s_hash_tab, esp_ptr_internal(s_hash_tab) ? "YES" : "NO");
    }
    if (!s_hash_lru) {
        s_hash_lru = (uint8_t *)heap_caps_calloc(JIT_HASH_SETS, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_hash_lru) s_hash_lru = (uint8_t *)heap_caps_calloc(JIT_HASH_SETS, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hash_lru) s_hash_lru = (uint8_t *)calloc(JIT_HASH_SETS, 1);
    }
    if (!s_hotspot_tag) {
        s_hotspot_tag = (uint32_t *)heap_caps_calloc(JIT_HOTSPOT_SIZE, sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_hotspot_tag) s_hotspot_tag = (uint32_t *)heap_caps_calloc(JIT_HOTSPOT_SIZE, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hotspot_tag) s_hotspot_tag = (uint32_t *)calloc(JIT_HOTSPOT_SIZE, sizeof(uint32_t));
    }
    if (!s_hotspot_cnt) {
        s_hotspot_cnt = (uint8_t *)heap_caps_calloc(JIT_HOTSPOT_SIZE, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_hotspot_cnt) s_hotspot_cnt = (uint8_t *)heap_caps_calloc(JIT_HOTSPOT_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hotspot_cnt) s_hotspot_cnt = (uint8_t *)calloc(JIT_HOTSPOT_SIZE, 1);
    }
#else
    if (!s_hash_tab) s_hash_tab = (JITEntry (*)[JIT_HASH_WAYS])calloc(JIT_HASH_SETS * JIT_HASH_WAYS, sizeof(JITEntry));
    if (!s_hash_lru) s_hash_lru = (uint8_t *)calloc(JIT_HASH_SETS, 1);
    if (!s_hotspot_tag) s_hotspot_tag = (uint32_t *)calloc(JIT_HOTSPOT_SIZE, sizeof(uint32_t));
    if (!s_hotspot_cnt) s_hotspot_cnt = (uint8_t *)calloc(JIT_HOTSPOT_SIZE, 1);
#endif

    if (!jit_smc_page_map) jit_smc_page_map = calloc(JIT_SMC_MAP_PAGES, 1);
    if (!jit_page_gen) jit_page_gen = calloc(JIT_SMC_MAP_PAGES, 1);
    if (!s_entry_gen) s_entry_gen = (uint8_t *)calloc(JIT_HASH_SETS * JIT_HASH_WAYS, 1);

    /* Inline flat-TLB. It must live in on-chip SRAM: routing a probe through
     * PSRAM would pay MSPI latency plus cache-miss penalties and swallow the
     * win the inline path is meant to deliver. 512 * 8 bytes = 4KB. */
    if (!jit_fast_tlb) {
#ifdef BUILD_ESP32
        jit_fast_tlb = (JitFastTLBEntry *)heap_caps_malloc(
            sizeof(JitFastTLBEntry) * JIT_FAST_TLB_SIZE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!jit_fast_tlb) {
            jit_fast_tlb = (JitFastTLBEntry *)heap_caps_malloc(
                sizeof(JitFastTLBEntry) * JIT_FAST_TLB_SIZE, MALLOC_CAP_8BIT);
        }
#else
        jit_fast_tlb = (JitFastTLBEntry *)malloc(
            sizeof(JitFastTLBEntry) * JIT_FAST_TLB_SIZE);
#endif
        if (jit_fast_tlb) {
            memset(jit_fast_tlb, 0xFF, sizeof(JitFastTLBEntry) * JIT_FAST_TLB_SIZE);
#ifdef BUILD_ESP32
            printf("[JIT] Flat-TLB %u entries @ %p (internal SRAM: %s)\n",
                   (unsigned)JIT_FAST_TLB_SIZE, (void *)jit_fast_tlb,
                   esp_ptr_internal(jit_fast_tlb) ? "YES" : "NO");
#endif
        }
    }

    s_jit_off = 0;
    if (s_hash_tab) memset(s_hash_tab, 0, JIT_HASH_SETS * JIT_HASH_WAYS * sizeof(JITEntry));
    if (s_hash_lru) memset(s_hash_lru, 0, JIT_HASH_SETS);
    if (s_hotspot_tag) memset(s_hotspot_tag, 0, JIT_HOTSPOT_SIZE * sizeof(uint32_t));
    if (s_hotspot_cnt) memset(s_hotspot_cnt, 0, JIT_HOTSPOT_SIZE);
    if (s_entry_gen) memset(s_entry_gen, 0, JIT_HASH_SETS * JIT_HASH_WAYS);
    jit_fast_tlb_active = jit_fast_tlb != NULL;
    s_initialized = true;
}

void jit_flush(void) {
    if (!s_initialized) return;
    s_jit_off = 0;
    if (s_hash_tab) {
        memset(s_hash_tab, 0, JIT_HASH_SETS * JIT_HASH_WAYS * sizeof(JITEntry));
    }
    if (s_hash_lru) {
        memset(s_hash_lru, 0, JIT_HASH_SETS);
    }
    if (s_hotspot_tag) {
        memset(s_hotspot_tag, 0, JIT_HOTSPOT_SIZE * sizeof(uint32_t));
    }
    if (s_hotspot_cnt) {
        memset(s_hotspot_cnt, 0, JIT_HOTSPOT_SIZE);
    }
    if (jit_smc_page_map) memset(jit_smc_page_map, 0, JIT_SMC_MAP_PAGES);
    if (jit_page_gen) memset(jit_page_gen, 0, JIT_SMC_MAP_PAGES);
    if (s_entry_gen) memset(s_entry_gen, 0, JIT_HASH_SETS * JIT_HASH_WAYS);
    jit_smc_dirty = 0;
    jit_fast_tlb_flush(); /* blocks retired: no resident mapping may survive */
    s_xlate_epoch++; /* every static chain link must be revalidated */
    jit_flush_cache();
}

void *jit_lookup(CPUI386 *cpu, uint32_t guest_pc) {
    if (!s_jit_enabled || !s_initialized || !s_hash_tab) return NULL;
    if (jit_smc_dirty) jit_flush(); /* only set on 8-bit page-gen wrap-around */
    bool ok; uint32_t key = jit_key_of(cpu, guest_pc, &ok);
    if (!ok) return NULL;
    uint32_t set = jit_hash(guest_pc, key);
    uint8_t gen = jit_page_gen_of(key);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        JITEntry *e = &s_hash_tab[set][way];
        if (e->phys_and_insns != 0 && (e->phys_and_insns & 0xFFFu) != 0 &&
            e->guest_pc == guest_pc && (e->phys_and_insns >> 12) == key &&
            s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
            s_jit_exec_count++;
            if (s_hash_lru) s_hash_lru[set] = 1 - way;
            return jit_entry_fn(e);
        }
    }
    return NULL;
}

void *jit_lookup_ex(CPUI386 *cpu, uint32_t guest_pc, uint32_t *num_insns) {
    if (num_insns) *num_insns = 1;
    if (!s_jit_enabled || !s_initialized || !s_hash_tab) return NULL;
    if (jit_smc_dirty) jit_flush(); /* only set on 8-bit page-gen wrap-around */
    bool ok; uint32_t key = jit_key_of(cpu, guest_pc, &ok);
    if (!ok) return NULL;
    uint32_t set = jit_hash(guest_pc, key);
    uint8_t gen = jit_page_gen_of(key);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        JITEntry *e = &s_hash_tab[set][way];
        if (e->phys_and_insns != 0 && (e->phys_and_insns & 0xFFFu) != 0 &&
            e->guest_pc == guest_pc && (e->phys_and_insns >> 12) == key &&
            s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
            s_jit_exec_count++;
            if (s_hash_lru) s_hash_lru[set] = 1 - way;
            if (num_insns) {
                uint32_t n = e->phys_and_insns & 0xFFFu;
                *num_insns = (n > 0) ? n : 1;
            }
            return jit_entry_fn(e);
        }
    }
    return NULL;
}

static void jit_insert_block(CPUI386 *cpu, uint32_t guest_pc, void *fn, uint32_t num_insns) {
    if (!s_hash_tab) return;
    bool ok; uint32_t key = jit_key_of(cpu, guest_pc, &ok);
    if (!ok) return;
    uint32_t set = jit_hash(guest_pc, key);
    int target_way = -1;

    /* 1. Check if guest_pc+phys key already present in this set */
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        JITEntry *e = &s_hash_tab[set][way];
        if (e->phys_and_insns != 0 && e->guest_pc == guest_pc && (e->phys_and_insns >> 12) == key) {
            target_way = way;
            break;
        }
    }
    /* 2. Otherwise find an empty slot (phys_and_insns == 0) */
    if (target_way < 0) {
        for (int way = 0; way < JIT_HASH_WAYS; way++) {
            if (s_hash_tab[set][way].phys_and_insns == 0) {
                target_way = way;
                break;
            }
        }
    }
    /* 3. Otherwise evict using LRU */
    if (target_way < 0) {
        target_way = (s_hash_lru != NULL) ? (s_hash_lru[set] & (JIT_HASH_WAYS - 1)) : 0;
    }

    s_hash_tab[set][target_way].guest_pc = guest_pc;
    s_hash_tab[set][target_way].phys_and_insns = jit_pack_tag(key, num_insns);
    s_hash_tab[set][target_way].code_off = (fn != NULL) ? (uint32_t)((uint8_t *)fn - s_jit_buf) : 0;
    if (s_entry_gen) s_entry_gen[set * JIT_HASH_WAYS + target_way] = jit_page_gen_of(key);
    if (s_hash_lru != NULL) {
        s_hash_lru[set] = 1 - target_way;
    }
}

bool jit_should_compile(CPUI386 *cpu, uint32_t guest_pc) {
    if (!s_jit_enabled) return false;
    /* Skip BIOS ROM (0xC0000..0xFFFFF), SeaBIOS relocated init (0xFB0000..0xFFFFFF), and high BIOS (>= 0xFFF00000) */
    if ((guest_pc >= 0x000C0000 && guest_pc < 0x00100000) ||
        (guest_pc >= 0x00FB0000 && guest_pc < 0x01000000) ||
        guest_pc >= 0xFFF00000) {
        return false;
    }
    /* Experimental: allow Windows 95 Ring 0 kernel space (>= 0xC0000000) to be
       translated as well. Controlled by TINY386_JIT_RING0=0 to restore the old
       conservative behaviour of running VMM32/VxD on the interpreter. */
    if (guest_pc >= 0xC0000000 && s_jit_ring0_allowed == 0) {
        return false;
    }
    if (!s_initialized) jit_init();
    bool ok; uint32_t key = jit_key_of(cpu, guest_pc, &ok);
    if (!ok) return false;
    uint32_t set = jit_hash(guest_pc, key);
    uint8_t gen = jit_page_gen_of(key);
    if (s_hash_tab) {
        for (int way = 0; way < JIT_HASH_WAYS; way++) {
            JITEntry *e = &s_hash_tab[set][way];
            /* A stale entry (page generation mismatch) must not block recompilation. */
            if (e->phys_and_insns != 0 && e->guest_pc == guest_pc && (e->phys_and_insns >> 12) == key &&
                s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
                return false;
            }
        }
    }
    uint32_t idx = (guest_pc * 2654435761u) >> (32 - JIT_HOTSPOT_SHIFT);
    if (s_hotspot_tag[idx] != guest_pc) {
        s_hotspot_tag[idx] = guest_pc;
        s_hotspot_cnt[idx] = 1;
        return (JIT_HOTSPOT_THRESHOLD <= 1);
    }
    if (s_hotspot_cnt[idx] < 255) {
        s_hotspot_cnt[idx]++;
    }
    return (s_hotspot_cnt[idx] >= JIT_HOTSPOT_THRESHOLD);
}

/* --- Code Generator for Entry/Exit Contract --- */
static void emit_prologue(uint32_t **buf) {
    emit_addi(buf, REG_SP, REG_SP, -64);
    emit_sw(buf, REG_RA, REG_SP, 44);
    emit_sw(buf, REG_S0, REG_SP, 40);
    emit_sw(buf, REG_S1, REG_SP, 36);
    emit_sw(buf, REG_S2, REG_SP, 32);
    emit_sw(buf, REG_S3, REG_SP, 28);
    emit_sw(buf, REG_S4, REG_SP, 24);
    emit_sw(buf, REG_S5, REG_SP, 20);
    emit_sw(buf, REG_S6, REG_SP, 16);
    emit_sw(buf, REG_S7, REG_SP, 12);
    emit_sw(buf, REG_S8, REG_SP, 8);

    /* s0 = a0 (CPUI386 *cpu) */
    emit_addi(buf, REG_S0, REG_A0, 0);

    /* Cache the two addresses a chain slot needs to validate a link, so each
     * slot costs a single load instead of a 2-instruction address materialise. */
    emit_mov_imm32(buf, REG_T0, (uint32_t)(uintptr_t)&s_xlate_epoch);
    emit_sw(buf, REG_T0, REG_SP, JIT_SLOT_GUARD);
    emit_mov_imm32(buf, REG_T0, (uint32_t)(uintptr_t)&jit_page_gen);
    emit_lw(buf, REG_T0, REG_T0, 0);              /* t0 = jit_page_gen array */
    emit_sw(buf, REG_T0, REG_SP, JIT_SLOT_PAGEGEN);

    /* Cache the inline flat-TLB base pointer (loaded indirectly so a block is
     * correct even if the table is (re)allocated later). */
    emit_mov_imm32(buf, REG_T0, (uint32_t)(uintptr_t)&jit_fast_tlb);
    emit_lw(buf, REG_T0, REG_T0, 0);
    emit_sw(buf, REG_T0, REG_SP, JIT_SLOT_TLB);

    /* Load x86 GPRs: gprx[0..7] */
    for (int i = 0; i < 8; i++) {
        emit_lw(buf, x86_to_rv[i], REG_S0, i * 4);
    }
}

/* emit_prologue() emits a fixed-length sequence; a chain slot jumps straight to
 * a successor's body, i.e. to code_off + jit_prologue_bytes(). The value is
 * measured from the emitter itself so it can never drift out of sync. */
static size_t s_jit_prologue_bytes = 0;
static size_t jit_prologue_bytes(void)
{
    if (!s_jit_prologue_bytes) {
        uint32_t tmp[64];
        uint32_t *p = tmp;
        emit_prologue(&p);
        s_jit_prologue_bytes = (size_t)((uint8_t *)p - (uint8_t *)tmp);
    }
    return s_jit_prologue_bytes;
}

/* Add `n` retired guest instructions to cpu->cycle (a 32-bit `long` on RV32) so
 * that a chained run can span many blocks while the dispatcher recovers the
 * consumed step count from the cycle delta alone. */
static void emit_cycle_add(uint32_t **buf, uint32_t n) {
    emit_lw(buf, REG_T3, REG_S0, CPU_OFF_CYCLE);
    if (n) emit_addi(buf, REG_T3, REG_T3, (int32_t)n);
    emit_sw(buf, REG_T3, REG_S0, CPU_OFF_CYCLE);
}

static void emit_epilogue(uint32_t **buf, uint32_t exit_eip) {
    /* Write back updated x86 GPRs */
    for (int i = 0; i < 8; i++) {
        emit_sw(buf, x86_to_rv[i], REG_S0, i * 4);
    }

    /* Update cpu->ip (offset 32) and cpu->next_ip (offset 36) */
    emit_mov_imm32(buf, REG_T0, exit_eip);
    emit_sw(buf, REG_T0, REG_S0, 32);
    emit_sw(buf, REG_T0, REG_S0, 36);

    /* Ensure cpu->cc.mask (offset 288) is 0 so interpreter reads cpu->flags directly */
    emit_sw(buf, REG_ZERO, REG_S0, 288);

    /* Restore callee-saved registers */
    emit_lw(buf, REG_RA, REG_SP, 44);
    emit_lw(buf, REG_S0, REG_SP, 40);
    emit_lw(buf, REG_S1, REG_SP, 36);
    emit_lw(buf, REG_S2, REG_SP, 32);
    emit_lw(buf, REG_S3, REG_SP, 28);
    emit_lw(buf, REG_S4, REG_SP, 24);
    emit_lw(buf, REG_S5, REG_SP, 20);
    emit_lw(buf, REG_S6, REG_SP, 16);
    emit_lw(buf, REG_S7, REG_SP, 12);
    emit_lw(buf, REG_S8, REG_SP, 8);
    emit_addi(buf, REG_SP, REG_SP, 64);

    emit_ret(buf);
}

/* Tail exit with a dynamic (register) target, e.g. RET. Not chainable: the
 * successor is only known at run time, so this is a terminal exit. */
static void emit_epilogue_reg(uint32_t **buf, int rv_reg_eip, uint32_t n) {
    emit_cycle_add(buf, n);

    /* Write back updated x86 GPRs */
    for (int i = 0; i < 8; i++) {
        emit_sw(buf, x86_to_rv[i], REG_S0, i * 4);
    }

    /* Update cpu->ip and cpu->next_ip from register */
    emit_sw(buf, rv_reg_eip, REG_S0, 32);
    emit_sw(buf, rv_reg_eip, REG_S0, 36);

    /* Ensure cpu->cc.mask (offset 288) is 0 so interpreter reads cpu->flags directly */
    emit_sw(buf, REG_ZERO, REG_S0, 288);

    /* Restore callee-saved registers */
    emit_lw(buf, REG_RA, REG_SP, 44);
    emit_lw(buf, REG_S0, REG_SP, 40);
    emit_lw(buf, REG_S1, REG_SP, 36);
    emit_lw(buf, REG_S2, REG_SP, 32);
    emit_lw(buf, REG_S3, REG_SP, 28);
    emit_lw(buf, REG_S4, REG_SP, 24);
    emit_lw(buf, REG_S5, REG_SP, 20);
    emit_lw(buf, REG_S6, REG_SP, 16);
    emit_lw(buf, REG_S7, REG_SP, 12);
    emit_lw(buf, REG_S8, REG_SP, 8);
    emit_addi(buf, REG_SP, REG_SP, 64);

    emit_ret(buf);
}

/*
 * Checks A0 after cpu_load32/store32:
 * If A0 != 0 (success): branches over the fault epilogue to next instruction.
 * If A0 == 0 (fault): falls through into fault epilogue (exiting with faulting EIP).
 */
static void emit_mem_fault_check(uint32_t **buf, uint32_t insn_eip) {
    uint32_t *branch_success = *buf;
    emit_bne(buf, REG_A0, REG_ZERO, 0); /* placeholder */
    emit_epilogue(buf, insn_eip);
    uint32_t *success_target = *buf;
    int32_t offset = (int32_t)((uint8_t *)success_target - (uint8_t *)branch_success);
    uint32_t *p = branch_success;
    emit_bne(&p, REG_A0, REG_ZERO, offset);
}

/* Inline flat-TLB 32-bit load. The effective address must already be in REG_A2.
 * On a table hit this is a single native lw straight into dst_reg; every other
 * case (non-flat segment, misaligned offset, table miss, no fast-TLB) branches
 * to a shared cpu_load32() slow path that also refills the table, then copies
 * the result into dst_reg.  dst_reg must avoid REG_T1..REG_T6, which the probe
 * uses as scratch. */
static void emit_mem_load32(uint32_t **buf, int dst_reg, int seg, uint32_t insn_eip) {
    if (jit_fast_tlb) {
        uint32_t *miss_bp[4]; int miss_rs1[4], miss_rs2[4]; int n_miss = 0;
        uint32_t *fast_jump;
        if (seg != 1 /*SEG_CS*/ && seg != 2 /*SEG_SS*/) {
            /* Only CS/SS are guaranteed flat by the dispatcher gate; other
             * segments must prove it at runtime. */
            emit_lw(buf, REG_T5, REG_S0, CPU_OFF_SEG_BASE(seg));
            miss_bp[n_miss] = *buf; miss_rs1[n_miss] = REG_T5; miss_rs2[n_miss] = REG_ZERO; n_miss++;
            emit_bne(buf, REG_T5, REG_ZERO, 0);
        }
        /* A misaligned offset would fault the native lw (and may straddle a
         * page), so it stays on the C path. */
        emit_andi(buf, REG_T5, REG_A2, 3);
        miss_bp[n_miss] = *buf; miss_rs1[n_miss] = REG_T5; miss_rs2[n_miss] = REG_ZERO; n_miss++;
        emit_bne(buf, REG_T5, REG_ZERO, 0);

        emit_lw(buf, REG_T6, REG_SP, JIT_SLOT_TLB);
        emit_srli(buf, REG_T1, REG_A2, 12);              /* vpage */
        emit_andi(buf, REG_T2, REG_T1, JIT_FAST_TLB_MASK);
        emit_slli(buf, REG_T2, REG_T2, 3);               /* 8-byte entries */
        emit_add(buf, REG_T2, REG_T2, REG_T6);           /* &entry */
        emit_lw(buf, REG_T3, REG_T2, 0);                 /* resident tag */
        miss_bp[n_miss] = *buf; miss_rs1[n_miss] = REG_T1; miss_rs2[n_miss] = REG_T3; n_miss++;
        emit_bne(buf, REG_T1, REG_T3, 0);
        emit_lw(buf, REG_T4, REG_T2, 4);                 /* host_delta */
        emit_add(buf, REG_T4, REG_T4, REG_A2);           /* host ptr = vaddr + delta */
        emit_lw(buf, dst_reg, REG_T4, 0);
        fast_jump = *buf;
        emit_jal(buf, REG_ZERO, 0);                      /* -> done */

        /* ---- slow path: all misses converge here ---- */
        uint32_t *slow = *buf;
        emit_addi(buf, REG_A0, REG_S0, 0);
        emit_addi(buf, REG_A1, REG_ZERO, seg);
        emit_addi(buf, REG_A3, REG_SP, JIT_SLOT_LOAD);
        emit_mov_imm32(buf, REG_T0, (uint32_t)cpu_load32);
        emit_jalr(buf, REG_RA, REG_T0, 0);
        emit_mem_fault_check(buf, insn_eip);
        emit_lw(buf, dst_reg, REG_SP, JIT_SLOT_LOAD);

        uint32_t *done = *buf;
        for (int i = 0; i < n_miss; i++) {
            emit_branch(&miss_bp[i], 0x1, miss_rs1[i], miss_rs2[i],
                        (int32_t)((uint8_t *)slow - (uint8_t *)miss_bp[i]));
        }
        emit_jal(&fast_jump, REG_ZERO, (int32_t)((uint8_t *)done - (uint8_t *)fast_jump));
    } else {
        /* Load: cpu_load32(cpu, seg, addr, SP + JIT_SLOT_LOAD) */
        emit_addi(buf, REG_A0, REG_S0, 0);
        emit_addi(buf, REG_A1, REG_ZERO, seg);
        emit_addi(buf, REG_A3, REG_SP, JIT_SLOT_LOAD);
        emit_mov_imm32(buf, REG_T0, (uint32_t)cpu_load32);
        emit_jalr(buf, REG_RA, REG_T0, 0);
        emit_mem_fault_check(buf, insn_eip);
        emit_lw(buf, dst_reg, REG_SP, JIT_SLOT_LOAD);
    }
}

/* 32-bit store. The effective address must already be in REG_A2 and the data in
 * src_reg (the copy into A3 happens before T0 is reloaded, so A4/T5 are legal
 * src_reg values).
 *
 * This deliberately has NO inline flat-TLB fast path. A flat-TLB entry caches
 * only a VA->PA delta, so it carries neither the PTE R/W permission nor any way
 * to perform the dirty-bit write-back that the interpreter does on *every*
 * store (translate_lpgno(), i386.c). A page that a load pre-filled while it was
 * read-only for this cpl (a shared Win9x DLL page, or a copy-on-write page)
 * would then accept a raw physical write straight through the fast path,
 * skipping the #PF that VMM32 needs in order to copy the page. Stores always
 * run cpu_store32(), which re-checks permission and sets the dirty bit. */
static void emit_mem_store32(uint32_t **buf, int src_reg, int seg, uint32_t insn_eip) {
    /* Store: cpu_store32(cpu, seg, addr, val) */
    emit_addi(buf, REG_A0, REG_S0, 0);
    emit_addi(buf, REG_A1, REG_ZERO, seg);
    emit_addi(buf, REG_A3, src_reg, 0);
    emit_mov_imm32(buf, REG_T0, (uint32_t)cpu_store32);
    emit_jalr(buf, REG_RA, REG_T0, 0);
    emit_mem_fault_check(buf, insn_eip);
}

/* Re-encode a placeholder branch in place once its real offset is known. */
static inline void emit_branch_patch(uint32_t *insn, int funct3, int rs1, int rs2,
                                     int32_t offset) {
    uint32_t *p = insn;
    emit_branch(&p, funct3, rs1, rs2, offset);
}

/*
 * Chainable block tail. `succ_body_off` is the buffer offset of the successor
 * block's body (past its prologue), or JIT_NO_CHAIN when the successor has no
 * compiled (and still valid) block yet. The successor's physical page identity
 * (`succ_key`) and the page generation observed when the link was created
 * (`succ_gen`) guard against guest self-modifying code; `link_epoch` is the
 * mapping epoch at link time and guards against a VA->PA change (only when the
 * successor is a different block, since a self-loop's own mapping was already
 * validated by the lookup that entered it).
 *
 * The link is deliberately not patched or unlinked afterwards: if any guard
 * fails the tail simply falls through into the normal exit body, and the
 * dispatcher re-validates and re-dispatches the successor. jit_flush() is safe
 * because it retires every block (and bumps the epoch), so no stale slot can be
 * reached.
 */
static void emit_tail(uint32_t **buf, uint32_t exit_eip, uint32_t n,
                      uint32_t succ_body_off, uint32_t succ_key, uint8_t succ_gen,
                      uint32_t link_epoch, bool self_link)
{
    emit_cycle_add(buf, n);

    uint32_t *bails[4];
    int bf3[4], br1[4], br2[4];
    int nbail = 0;

    /* Stop once this chain session has consumed the whole step budget. */
    emit_mov_imm32(buf, REG_T0, (uint32_t)(uintptr_t)&jit_cycle_deadline);
    emit_lw(buf, REG_T4, REG_T0, 0);              /* t4 = deadline */
    emit_lw(buf, REG_T3, REG_S0, CPU_OFF_CYCLE);
    bails[nbail] = *buf; bf3[nbail] = 0x5; br1[nbail] = REG_T3; br2[nbail] = REG_T4;
    emit_bge(buf, REG_T3, REG_T4, 0); nbail++;

    /* Stop when an interrupt is pending so pc_step can service it. */
    emit_lw(buf, REG_T5, REG_S0, CPU_OFF_INTR);
    bails[nbail] = *buf; bf3[nbail] = 0x1; br1[nbail] = REG_T5; br2[nbail] = REG_ZERO;
    emit_bne(buf, REG_T5, REG_ZERO, 0); nbail++;

    if (succ_body_off != JIT_NO_CHAIN) {
        if (!self_link) {
            /* Mapping epoch unchanged since the link was created. */
            emit_lw(buf, REG_T5, REG_SP, JIT_SLOT_GUARD);  /* t5 = &s_xlate_epoch */
            emit_lw(buf, REG_T5, REG_T5, 0);               /* t5 = epoch */
            emit_mov_imm32(buf, REG_T0, link_epoch);
            bails[nbail] = *buf; bf3[nbail] = 0x1; br1[nbail] = REG_T5; br2[nbail] = REG_T0;
            emit_bne(buf, REG_T5, REG_T0, 0); nbail++;
        }
        /* Successor's page generation unchanged (no self-modifying code). */
        emit_lw(buf, REG_T6, REG_SP, JIT_SLOT_PAGEGEN);    /* t6 = jit_page_gen base */
        emit_mov_imm32(buf, REG_T0, succ_key);
        emit_add(buf, REG_T6, REG_T6, REG_T0);
        emit_lbu(buf, REG_T5, REG_T6, 0);
        emit_mov_imm32(buf, REG_T0, succ_gen);
        bails[nbail] = *buf; bf3[nbail] = 0x1; br1[nbail] = REG_T5; br2[nbail] = REG_T0;
        emit_bne(buf, REG_T5, REG_T0, 0); nbail++;

        /* Jump straight into the successor's body. An absolute jalr is used
         * because the code pool (4-16MB) is larger than the +-1MB range of a
         * J-type jump. */
        emit_mov_imm32(buf, REG_T0, (uint32_t)(uintptr_t)(s_jit_buf + succ_body_off));
        emit_jalr(buf, REG_ZERO, REG_T0, 0);
    }

    uint32_t *exit_body = *buf;
    for (int i = 0; i < nbail; i++) {
        emit_branch_patch(bails[i], bf3[i], br1[i], br2[i],
                          (int32_t)((uint8_t *)exit_body - (uint8_t *)bails[i]));
    }

    emit_epilogue(buf, exit_eip);
}

/*
 * Resolve a chain target at compile time. Returns true when `succ_pc` is the
 * block currently being compiled (self-loop, addressed via blk_start_off) or is
 * already present and still page-generation-valid in the block table. On success
 * the successor's physical page key, the page generation and mapping epoch
 * observed now, and the successor body's buffer offset are returned.
 */
static bool jit_resolve_chain(CPUI386 *cpu, uint32_t blk_start_off, uint32_t start_eip,
                              uint32_t succ_pc, uint32_t *key_out, uint8_t *gen_out,
                              uint32_t *epoch_out, uint32_t *body_off_out, bool *self_out)
{
    *epoch_out = s_xlate_epoch;
    *self_out = false;

    bool ok;
    uint32_t key = jit_key_of(cpu, succ_pc, &ok);
    if (!ok) return false;

    if (succ_pc == start_eip) {
        *key_out = key;
        *gen_out = jit_page_gen_of(key);
        *body_off_out = blk_start_off + (uint32_t)jit_prologue_bytes();
        *self_out = true;
        return true;
    }

    if (!s_hash_tab || !s_entry_gen) return false;
    uint32_t set = jit_hash(succ_pc, key);
    uint8_t gen = jit_page_gen_of(key);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        JITEntry *e = &s_hash_tab[set][way];
        if (e->phys_and_insns != 0 && (e->phys_and_insns & 0xFFFu) != 0 &&
            e->guest_pc == succ_pc && (e->phys_and_insns >> 12) == key &&
            s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
            *key_out = key;
            *gen_out = gen;
            *body_off_out = e->code_off + (uint32_t)jit_prologue_bytes();
            return true;
        }
    }
    return false;
}

/* Emit the tail for a normal (non-fault) block exit, chaining to the successor
 * block when one is already compiled for `exit_eip`. */
static void emit_block_tail(uint32_t **buf, uint32_t exit_eip, uint32_t n,
                            CPUI386 *cpu, uint32_t blk_start_off, uint32_t start_eip)
{
    uint32_t key = 0, epoch = 0, body_off = JIT_NO_CHAIN;
    uint8_t gen = 0;
    bool self = false;
    if (!jit_resolve_chain(cpu, blk_start_off, start_eip, exit_eip,
                           &key, &gen, &epoch, &body_off, &self)) {
        body_off = JIT_NO_CHAIN;
    }
    emit_tail(buf, exit_eip, n, body_off, key, gen, epoch, self);
}

static uint8_t jit_read_u8(CPUI386 *cpu, uint32_t eip) {
    uint8_t val = 0;
    cpu_load8(cpu, 1 /* SEG_CS */, eip, &val);
    return val;
}

static uint32_t jit_read_u32(CPUI386 *cpu, uint32_t eip) {
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = jit_read_u8(cpu, eip + i);
        val |= ((uint32_t)b) << (i * 8);
    }
    return val;
}

enum { CMP_NONE = 0, CMP_REG_REG, CMP_REG_IMM, CMP_TEST };

/**
 * @brief Dynamic Basic Block Compiler (Phase 6 Core)
 */
void *jit_compile_block(CPUI386 *cpu, uint32_t start_eip) {
    if (!s_initialized) jit_init();

    /* Reserve 1.5KB per block */
    if (s_jit_off + 1536 >= JIT_BUF_SIZE) {
        jit_flush();
    }

    uint32_t *code_start = (uint32_t *)(s_jit_buf + s_jit_off);
    uint32_t *code_ptr = code_start;
    uint32_t blk_start_off = (uint32_t)((uint8_t *)code_start - s_jit_buf);

    emit_prologue(&code_ptr);

    uint32_t cur_eip = start_eip;
    int insn_count = 0;
    int max_insns = 32;
    uint8_t last_op = 0;

    int last_cmp_type = CMP_NONE;
    int cmp_r1 = 0, cmp_r2 = 0;
    uint32_t cmp_imm = 0;

    while (insn_count < max_insns) {
        /* Keep every block inside a single 4KB page, with a 15-byte lookahead
         * guard so that no decoded instruction can itself span a page boundary.
         * The block's identity key is the physical page backing start_eip; if a
         * block reached into a second page, that page could later be remapped
         * while the first stayed put, and the stale block would be silently
         * reused because only the start page's physical address is rechecked. */
        if ((cur_eip >> 12) != (start_eip >> 12) ||
            (cur_eip & 0xFFFu) > 0xFF0u) {
            if (insn_count == 0) goto fallback_exit;
            goto block_done;
        }
        uint8_t op = jit_read_u8(cpu, cur_eip);
        last_op = op;

        /* The CMP/TEST operand shadows are only valid for the instruction that
           immediately follows the compare. Take a snapshot for this iteration
           and clear it so that any other instruction cannot reuse stale flags. */
        int prev_cmp = last_cmp_type;
        last_cmp_type = CMP_NONE;

        /* Don't compile blocks that start with a branch/ret (0 useful body instructions) */
        if (insn_count == 0 && (op == 0xEB || op == 0xE9 || (op >= 0x70 && op <= 0x7F) || op == 0xC3 || op == 0xC2)) {
            goto fallback_exit;
        }
        if (insn_count == 0 && op == 0x0F) {
            uint8_t op2 = jit_read_u8(cpu, cur_eip + 1);
            if (op2 >= 0x80 && op2 <= 0x8F) {
                goto fallback_exit;
            }
        }

        /* NOP (0x90) */
        if (op == 0x90) {
            cur_eip++;
            insn_count++;
            continue;
        }

        /* PUSH r32 (0x50 + reg) */
        if (op >= 0x50 && op <= 0x57) {
            int rd = op - 0x50;
            uint32_t insn_eip = cur_eip;
            emit_addi(&code_ptr, REG_A0, REG_S0, 0);
            emit_addi(&code_ptr, REG_A1, REG_ZERO, 2 /* SEG_SS */);
            emit_addi(&code_ptr, REG_A2, REG_S5, -4);
            emit_addi(&code_ptr, REG_A3, x86_to_rv[rd], 0);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_store32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_addi(&code_ptr, REG_S5, REG_S5, -4);
            cur_eip += 1;
            insn_count++;
            continue;
        }

        /* POP r32 (0x58 + reg) */
        if (op >= 0x58 && op <= 0x5F) {
            int rd = op - 0x58;
            uint32_t insn_eip = cur_eip;
            emit_addi(&code_ptr, REG_A0, REG_S0, 0);
            emit_addi(&code_ptr, REG_A1, REG_ZERO, 2 /* SEG_SS */);
            emit_addi(&code_ptr, REG_A2, REG_S5, 0);
            emit_addi(&code_ptr, REG_A3, REG_SP, JIT_SLOT_LOAD);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_lw(&code_ptr, x86_to_rv[rd], REG_SP, JIT_SLOT_LOAD);
            emit_addi(&code_ptr, REG_S5, REG_S5, 4);
            cur_eip += 1;
            insn_count++;
            continue;
        }

        /* CALL rel32 (0xE8) */
        if (op == 0xE8) {
            int32_t rel32 = (int32_t)jit_read_u32(cpu, cur_eip + 1);
            uint32_t insn_eip = cur_eip;
            uint32_t return_eip = cur_eip + 5;
            uint32_t target_eip = cur_eip + 5 + rel32;

            emit_addi(&code_ptr, REG_A0, REG_S0, 0);
            emit_addi(&code_ptr, REG_A1, REG_ZERO, 2 /* SEG_SS */);
            emit_addi(&code_ptr, REG_A2, REG_S5, -4);
            emit_mov_imm32(&code_ptr, REG_A3, return_eip);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_store32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_addi(&code_ptr, REG_S5, REG_S5, -4);
            cur_eip = target_eip;
            insn_count++;
            goto block_done;
        }

        /* RET (0xC3) */
        if (op == 0xC3) {
            uint32_t insn_eip = cur_eip;
            emit_addi(&code_ptr, REG_A0, REG_S0, 0);
            emit_addi(&code_ptr, REG_A1, REG_ZERO, 2 /* SEG_SS */);
            emit_addi(&code_ptr, REG_A2, REG_S5, 0);
            emit_addi(&code_ptr, REG_A3, REG_SP, JIT_SLOT_LOAD);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_lw(&code_ptr, REG_T1, REG_SP, JIT_SLOT_LOAD);
            emit_addi(&code_ptr, REG_S5, REG_S5, 4);
            insn_count++;
            emit_epilogue_reg(&code_ptr, REG_T1, (uint32_t)insn_count);
            goto block_finished;
        }

        /* RET imm16 (0xC2) */
        if (op == 0xC2) {
            uint16_t imm16 = jit_read_u8(cpu, cur_eip + 1) | (jit_read_u8(cpu, cur_eip + 2) << 8);
            uint32_t insn_eip = cur_eip;
            emit_addi(&code_ptr, REG_A0, REG_S0, 0);
            emit_addi(&code_ptr, REG_A1, REG_ZERO, 2 /* SEG_SS */);
            emit_addi(&code_ptr, REG_A2, REG_S5, 0);
            emit_addi(&code_ptr, REG_A3, REG_SP, JIT_SLOT_LOAD);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_lw(&code_ptr, REG_T1, REG_SP, JIT_SLOT_LOAD);
            emit_addi(&code_ptr, REG_S5, REG_S5, 4 + imm16);
            insn_count++;
            emit_epilogue_reg(&code_ptr, REG_T1, (uint32_t)insn_count);
            goto block_finished;
        }

        /* MOV r32, imm32 (0xB8 + reg) */
        if (op >= 0xB8 && op <= 0xBF) {
            int rd = op - 0xB8;
            uint32_t imm = jit_read_u32(cpu, cur_eip + 1);
            emit_mov_imm32(&code_ptr, x86_to_rv[rd], imm);
            cur_eip += 5;
            insn_count++;
            continue;
        }

        /* INC r32 (0x40 + reg) */
        if (op >= 0x40 && op <= 0x47) {
            int rd = op - 0x40;
            emit_addi(&code_ptr, x86_to_rv[rd], x86_to_rv[rd], 1);
            emit_addi(&code_ptr, REG_T1, x86_to_rv[rd], 0);
            emit_flags_inc(&code_ptr, x86_to_rv[rd]);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* DEC r32 (0x48 + reg) */
        if (op >= 0x48 && op <= 0x4F) {
            int rd = op - 0x48;
            emit_addi(&code_ptr, x86_to_rv[rd], x86_to_rv[rd], -1);
            emit_addi(&code_ptr, REG_T1, x86_to_rv[rd], 0);
            emit_flags_dec(&code_ptr, x86_to_rv[rd]);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* CWDE (0x98) - Sign-extend AX into EAX */
        if (op == 0x98) {
            emit_slli(&code_ptr, REG_S1, REG_S1, 16);
            emit_srai(&code_ptr, REG_S1, REG_S1, 16);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* CDQ (0x99) - Sign-extend EAX into EDX:EAX */
        if (op == 0x99) {
            emit_srai(&code_ptr, REG_S3, REG_S1, 31);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* XCHG r32, EAX (0x91 .. 0x97) */
        if (op >= 0x91 && op <= 0x97) {
            int rd = op - 0x90;
            emit_addi(&code_ptr, REG_T1, REG_S1, 0);
            emit_addi(&code_ptr, REG_S1, x86_to_rv[rd], 0);
            emit_addi(&code_ptr, x86_to_rv[rd], REG_T1, 0);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* 2-byte Opcode with ModR/M */
        uint8_t modrm = jit_read_u8(cpu, cur_eip + 1);
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t reg = (modrm >> 3) & 7;
        uint8_t rm  = modrm & 7;

        /* Register-to-Register operations (mod == 3) */
        if (mod == 3) {
            /* XCHG r/m32, r32 (0x87) */
            if (op == 0x87) {
                emit_addi(&code_ptr, REG_T1, x86_to_rv[rm], 0);
                emit_addi(&code_ptr, x86_to_rv[rm], x86_to_rv[reg], 0);
                emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* MOV r/m32, r32 (0x89) */
            if (op == 0x89) {
                emit_addi(&code_ptr, x86_to_rv[rm], x86_to_rv[reg], 0);
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* MOV r32, r/m32 (0x8B) */
            if (op == 0x8B) {
                emit_addi(&code_ptr, x86_to_rv[reg], x86_to_rv[rm], 0);
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* ADD r/m32, r32 (0x01) */
            if (op == 0x01) {
                emit_add(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_add(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* ADD r32, r/m32 (0x03) */
            if (op == 0x03) {
                emit_add(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_flags_add(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* SUB r/m32, r32 (0x29) */
            if (op == 0x29) {
                emit_sub(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* SUB r32, r/m32 (0x2B) */
            if (op == 0x2B) {
                emit_sub(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* AND r/m32, r32 (0x21) */
            if (op == 0x21) {
                emit_and(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_logic(&code_ptr, REG_T1);
                emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* AND r32, r/m32 (0x23) */
            if (op == 0x23) {
                emit_and(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_flags_logic(&code_ptr, REG_T1);
                emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* OR r/m32, r32 (0x09) */
            if (op == 0x09) {
                emit_or(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_logic(&code_ptr, REG_T1);
                emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* OR r32, r/m32 (0x0B) */
            if (op == 0x0B) {
                emit_or(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_flags_logic(&code_ptr, REG_T1);
                emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* XOR r/m32, r32 (0x31) */
            if (op == 0x31) {
                emit_xor(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_logic(&code_ptr, REG_T1);
                emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* XOR r32, r/m32 (0x33) */
            if (op == 0x33) {
                emit_xor(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_flags_logic(&code_ptr, REG_T1);
                emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* CMP r/m32, r32 (0x39) */
            if (op == 0x39) {
                emit_sub(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                last_cmp_type = CMP_REG_REG;
                cmp_r1 = x86_to_rv[rm];
                cmp_r2 = x86_to_rv[reg];
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* CMP r32, r/m32 (0x3B) */
            if (op == 0x3B) {
                emit_sub(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[reg], x86_to_rv[rm]);
                last_cmp_type = CMP_REG_REG;
                cmp_r1 = x86_to_rv[reg];
                cmp_r2 = x86_to_rv[rm];
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* TEST r/m32, r32 (0x85) */
            if (op == 0x85) {
                emit_and(&code_ptr, REG_T1, x86_to_rv[rm], x86_to_rv[reg]);
                emit_flags_logic(&code_ptr, REG_T1);
                last_cmp_type = CMP_TEST;
                cur_eip += 2;
                insn_count++;
                continue;
            }

            /* Group 1 Immediate: 0x83 r/m32, imm8 */
            if (op == 0x83) {
                int8_t simm8 = (int8_t)jit_read_u8(cpu, cur_eip + 2);
                int subop = reg;
                switch (subop) {
                case 0: /* ADD */
                    emit_mov_imm32(&code_ptr, REG_T5, (uint32_t)(int32_t)simm8);
                    emit_add(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_add(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 1: /* OR */
                    emit_ori(&code_ptr, REG_T1, x86_to_rv[rm], simm8);
                    emit_flags_logic(&code_ptr, REG_T1);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 4: /* AND */
                    emit_andi(&code_ptr, REG_T1, x86_to_rv[rm], simm8);
                    emit_flags_logic(&code_ptr, REG_T1);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 5: /* SUB */
                    emit_mov_imm32(&code_ptr, REG_T5, (uint32_t)(int32_t)simm8);
                    emit_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 6: /* XOR */
                    emit_xori(&code_ptr, REG_T1, x86_to_rv[rm], simm8);
                    emit_flags_logic(&code_ptr, REG_T1);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 7: /* CMP */
                    emit_mov_imm32(&code_ptr, REG_T5, (uint32_t)(int32_t)simm8);
                    emit_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    last_cmp_type = CMP_REG_IMM;
                    cmp_r1 = x86_to_rv[rm];
                    cmp_imm = (uint32_t)(int32_t)simm8;
                    break;
                default:
                    goto fallback_exit;
                }
                cur_eip += 3;
                insn_count++;
                continue;
            }

            /* Group 1 Immediate: 0x81 r/m32, imm32 */
            if (op == 0x81) {
                uint32_t imm32 = jit_read_u32(cpu, cur_eip + 2);
                int subop = reg;
                switch (subop) {
                case 0: /* ADD */
                    emit_mov_imm32(&code_ptr, REG_T5, imm32);
                    emit_add(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_add(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 1: /* OR */
                    emit_mov_imm32(&code_ptr, REG_T5, imm32);
                    emit_or(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_logic(&code_ptr, REG_T1);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 4: /* AND */
                    emit_mov_imm32(&code_ptr, REG_T5, imm32);
                    emit_and(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_logic(&code_ptr, REG_T1);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 5: /* SUB */
                    emit_mov_imm32(&code_ptr, REG_T5, imm32);
                    emit_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 6: /* XOR */
                    emit_mov_imm32(&code_ptr, REG_T5, imm32);
                    emit_xor(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_logic(&code_ptr, REG_T1);
                    emit_addi(&code_ptr, x86_to_rv[rm], REG_T1, 0);
                    last_cmp_type = CMP_NONE;
                    break;
                case 7: /* CMP */
                    emit_mov_imm32(&code_ptr, REG_T5, imm32);
                    emit_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                    last_cmp_type = CMP_REG_IMM;
                    cmp_r1 = x86_to_rv[rm];
                    cmp_imm = imm32;
                    break;
                default:
                    goto fallback_exit;
                }
                cur_eip += 6;
                insn_count++;
                continue;
            }

            /* Group 2 Shift / Rotate (0xD1, 0xC1, 0xD3):
             * Handled by interpreter fallback:
             * x86 shift/rotates have subtle EFLAGS requirements:
             * 1) Count == 0 leaves all flags unchanged (must not clobber EFLAGS)
             * 2) Shift sets CF to the last bit shifted out (critical for wsprintf formatting)
             * 3) ROL/ROR preserve ZF, SF, PF, AF untouched
             * Falling back to interpreter provides 100% precision with negligible (<1%) speed difference.
             */


            /* IMUL r32, r/m32, imm8 (0x6B) */
            if (op == 0x6B) {
                int8_t simm8 = (int8_t)jit_read_u8(cpu, cur_eip + 2);
                emit_mov_imm32(&code_ptr, REG_T5, (uint32_t)(int32_t)simm8);
                emit_mul(&code_ptr, x86_to_rv[reg], x86_to_rv[rm], REG_T5);
                cur_eip += 3;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }

            /* IMUL r32, r/m32, imm32 (0x69) */
            if (op == 0x69) {
                uint32_t imm32 = jit_read_u32(cpu, cur_eip + 2);
                emit_mov_imm32(&code_ptr, REG_T5, imm32);
                emit_mul(&code_ptr, x86_to_rv[reg], x86_to_rv[rm], REG_T5);
                cur_eip += 6;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }

            /* Group 3: 0xF7 r/m32 (TEST, NOT, NEG, DIV, IDIV) */
            if (op == 0xF7) {
                int subop = reg;
                switch (subop) {
                case 0: /* TEST r/m32, imm32 */
                case 1: /* TEST r/m32, imm32 (alias) */
                    {
                        uint32_t imm32 = jit_read_u32(cpu, cur_eip + 2);
                        emit_mov_imm32(&code_ptr, REG_T5, imm32);
                        emit_and(&code_ptr, REG_T1, x86_to_rv[rm], REG_T5);
                        emit_flags_logic(&code_ptr, REG_T1);
                        last_cmp_type = CMP_TEST;
                        cur_eip += 6;
                        insn_count++;
                        continue;
                    }
                case 2: /* NOT r/m32 */
                    emit_xori(&code_ptr, x86_to_rv[rm], x86_to_rv[rm], -1);
                    cur_eip += 2;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                case 3: /* NEG r/m32: fallback to interpreter for exact flag semantics (CF/AF/PF) */
                    goto fallback_exit;
                case 6: /* DIV r/m32: fallback to interpreter for 64-bit EDX:EAX / rm and #DE */
                case 7: /* IDIV r/m32: fallback to interpreter for 64-bit EDX:EAX / rm and #DE */
                default:
                    goto fallback_exit;
                }
            }
        }

        /* Memory operations (mod < 3).  Three families share this block:
         *  - plain MOV r/m<->r32 (0x8B/0x89) and LEA (0x8D);
         *  - read-only ALU with a memory source (0x03/0x0B/0x23/0x2B/0x33/0x3B,
         *    CMP r/m32,r32 0x39 and TEST r/m32,r32 0x85);
         *  - read-modify-write with a memory destination (0x01/0x09/0x21/0x29/
         *    0x31 and the Group-1 immediate forms 0x81/0x83 /0../7). */
        if ((op == 0x89 || op == 0x8B || op == 0x8D ||
             op == 0x01 || op == 0x09 || op == 0x21 || op == 0x29 || op == 0x31 ||
             op == 0x03 || op == 0x0B || op == 0x23 || op == 0x2B ||
             op == 0x33 || op == 0x39 || op == 0x3B || op == 0x85 ||
             op == 0x81 || op == 0x83) && mod < 3) {
            int32_t disp = 0;
            int insn_len = 2;
            int seg = 3; /* Default SEG_DS */
            int base_rv = -1;
            bool valid_mem = false;
            bool has_sib = (rm == 4);
            uint8_t sib_scale = 0, sib_index = 4, sib_base = 0;

            if (has_sib) {
                uint8_t sib = jit_read_u8(cpu, cur_eip + 2);
                sib_scale = (sib >> 6) & 3;
                sib_index = (sib >> 3) & 7;
                sib_base  = sib & 7;

                if (mod == 0) {
                    if (sib_base == 5) {
                        /* [disp32 + index*scale] */
                        disp = (int32_t)jit_read_u32(cpu, cur_eip + 3);
                        insn_len = 7;
                        base_rv = -1;
                    } else {
                        /* [base + index*scale] */
                        disp = 0;
                        insn_len = 3;
                        base_rv = x86_to_rv[sib_base];
                    }
                    valid_mem = true;
                } else if (mod == 1) {
                    disp = (int8_t)jit_read_u8(cpu, cur_eip + 3);
                    insn_len = 4;
                    base_rv = x86_to_rv[sib_base];
                    valid_mem = true;
                } else if (mod == 2) {
                    disp = (int32_t)jit_read_u32(cpu, cur_eip + 3);
                    insn_len = 7;
                    base_rv = x86_to_rv[sib_base];
                    valid_mem = true;
                }
                /* Default segment for SIB: ESP(4) or EBP(5) uses SS, others DS */
                if (base_rv != -1 && (sib_base == 4 || sib_base == 5)) {
                    seg = 2; /* SEG_SS */
                } else {
                    seg = 3; /* SEG_DS */
                }
            } else {
                /* rm != 4: no SIB */
                if (mod == 0) {
                    if (rm == 5) {
                        /* [disp32] */
                        disp = (int32_t)jit_read_u32(cpu, cur_eip + 2);
                        insn_len = 6;
                        base_rv = -1;
                    } else {
                        /* [base] */
                        disp = 0;
                        insn_len = 2;
                        base_rv = x86_to_rv[rm];
                    }
                    valid_mem = true;
                } else if (mod == 1) {
                    disp = (int8_t)jit_read_u8(cpu, cur_eip + 2);
                    insn_len = 3;
                    base_rv = x86_to_rv[rm];
                    valid_mem = true;
                } else if (mod == 2) {
                    disp = (int32_t)jit_read_u32(cpu, cur_eip + 2);
                    insn_len = 6;
                    base_rv = x86_to_rv[rm];
                    valid_mem = true;
                }
                seg = (rm == 5 && mod != 0) ? 2 /* SEG_SS */ : 3 /* SEG_DS */;
            }

            /* Group-1 immediates (0x81/0x83) trail the ModR/M+SIB+disp, so the
             * displacement offsets above are unchanged — only the total length
             * grows.  Read the immediate before adjusting insn_len. */
            int imm_val = 0;
            if (op == 0x81 || op == 0x83) {
                if (op == 0x83) imm_val = (int32_t)(int8_t)jit_read_u8(cpu, cur_eip + insn_len);
                else            imm_val = (int32_t)jit_read_u32(cpu, cur_eip + insn_len);
                insn_len += (op == 0x83) ? 1 : 4;
            }

            if (valid_mem) {
                uint32_t insn_eip = cur_eip;

                /* Compute effective address into REG_A2 */
                if (has_sib && sib_index != 4) {
                    int index_rv = x86_to_rv[sib_index];
                    if (base_rv != -1) {
                        /* Zba acceleration: merge scaled index and base into one cycle */
                        if (sib_scale == 1) {
                            emit_sh1add(&code_ptr, REG_A2, index_rv, base_rv);
                        } else if (sib_scale == 2) {
                            emit_sh2add(&code_ptr, REG_A2, index_rv, base_rv);
                        } else if (sib_scale == 3) {
                            emit_sh3add(&code_ptr, REG_A2, index_rv, base_rv);
                        } else {
                            emit_add(&code_ptr, REG_A2, index_rv, base_rv);
                        }
                    } else {
                        if (sib_scale == 0) {
                            emit_addi(&code_ptr, REG_A2, index_rv, 0);
                        } else {
                            emit_slli(&code_ptr, REG_A2, index_rv, sib_scale);
                        }
                    }
                    if (disp != 0) {
                        if (disp >= -2048 && disp <= 2047) {
                            emit_addi(&code_ptr, REG_A2, REG_A2, disp);
                        } else {
                            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)disp);
                            emit_add(&code_ptr, REG_A2, REG_A2, REG_T0);
                        }
                    }
                } else {
                    if (base_rv == -1) {
                        emit_mov_imm32(&code_ptr, REG_A2, (uint32_t)disp);
                    } else if (disp == 0) {
                        emit_addi(&code_ptr, REG_A2, base_rv, 0);
                    } else if (disp >= -2048 && disp <= 2047) {
                        emit_addi(&code_ptr, REG_A2, base_rv, disp);
                    } else {
                        emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)disp);
                        emit_add(&code_ptr, REG_A2, base_rv, REG_T0);
                    }
                }

                if (op == 0x8D) {
                    /* LEA: destination register gets effective address directly, no memory access, no flag update */
                    emit_addi(&code_ptr, x86_to_rv[reg], REG_A2, 0);
                    cur_eip += insn_len;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                } else if (op == 0x8B) {
                    /* MOV r32, r/m32 */
                    emit_mem_load32(&code_ptr, x86_to_rv[reg], seg, insn_eip);
                } else if (op == 0x89) {
                    /* MOV r/m32, r32 */
                    emit_mem_store32(&code_ptr, x86_to_rv[reg], seg, insn_eip);
                } else if (op == 0x01 || op == 0x09 || op == 0x21 ||
                           op == 0x29 || op == 0x31) {
                    /* Read-modify-write r/m32, r32.  The EA in REG_A2 is spilled
                     * to JIT_SLOT_EA because the load slow path is a C call that
                     * clobbers the caller-saved registers (including a2).  The
                     * result is parked in REG_A4, which the store helper never
                     * scratches (it only touches t0-t6/a0-a3). */
                    emit_sw(&code_ptr, REG_A2, REG_SP, JIT_SLOT_EA);
                    emit_mem_load32(&code_ptr, REG_T5, seg, insn_eip);
                    switch (op) {
                    case 0x01: /* ADD r/m32, r32 */
                        emit_add(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_add(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        break;
                    case 0x09: /* OR r/m32, r32 */
                        emit_or(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_logic(&code_ptr, REG_T1);
                        break;
                    case 0x21: /* AND r/m32, r32 */
                        emit_and(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_logic(&code_ptr, REG_T1);
                        break;
                    case 0x29: /* SUB r/m32, r32 */
                        emit_sub(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_sub(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        break;
                    case 0x31: /* XOR r/m32, r32 */
                        emit_xor(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_logic(&code_ptr, REG_T1);
                        break;
                    }
                    emit_addi(&code_ptr, REG_A4, REG_T1, 0);
                    emit_lw(&code_ptr, REG_A2, REG_SP, JIT_SLOT_EA);
                    emit_mem_store32(&code_ptr, REG_A4, seg, insn_eip);
                    last_cmp_type = CMP_NONE;
                } else if (op == 0x81 || op == 0x83) {
                    /* Group-1 immediate with a memory destination.  /7 (CMP) is
                     * read-only; /0,/1,/4,/5,/6 are read-modify-write.  /2 (ADC)
                     * and /3 (SBB) need a carry-in the backend does not model, so
                     * they fall back like their register forms do.  The imm lives
                     * in REG_T6 (survives emit_flags_*, which clobber t0/t2-t4). */
                    int subop = reg;
                    if (subop == 7) {
                        emit_mem_load32(&code_ptr, REG_T5, seg, insn_eip);
                        emit_mov_imm32(&code_ptr, REG_T6, (uint32_t)imm_val);
                        emit_sub(&code_ptr, REG_T1, REG_T5, REG_T6);
                        emit_flags_sub(&code_ptr, REG_T1, REG_T5, REG_T6);
                        last_cmp_type = CMP_NONE;
                    } else if (subop == 0 || subop == 1 || subop == 4 ||
                               subop == 5 || subop == 6) {
                        emit_sw(&code_ptr, REG_A2, REG_SP, JIT_SLOT_EA);
                        emit_mem_load32(&code_ptr, REG_T5, seg, insn_eip);
                        emit_mov_imm32(&code_ptr, REG_T6, (uint32_t)imm_val);
                        switch (subop) {
                        case 0: /* ADD */
                            emit_add(&code_ptr, REG_T1, REG_T5, REG_T6);
                            emit_flags_add(&code_ptr, REG_T1, REG_T5, REG_T6);
                            break;
                        case 1: /* OR */
                            emit_or(&code_ptr, REG_T1, REG_T5, REG_T6);
                            emit_flags_logic(&code_ptr, REG_T1);
                            break;
                        case 4: /* AND */
                            emit_and(&code_ptr, REG_T1, REG_T5, REG_T6);
                            emit_flags_logic(&code_ptr, REG_T1);
                            break;
                        case 5: /* SUB */
                            emit_sub(&code_ptr, REG_T1, REG_T5, REG_T6);
                            emit_flags_sub(&code_ptr, REG_T1, REG_T5, REG_T6);
                            break;
                        case 6: /* XOR */
                            emit_xor(&code_ptr, REG_T1, REG_T5, REG_T6);
                            emit_flags_logic(&code_ptr, REG_T1);
                            break;
                        }
                        emit_addi(&code_ptr, REG_A4, REG_T1, 0);
                        emit_lw(&code_ptr, REG_A2, REG_SP, JIT_SLOT_EA);
                        emit_mem_store32(&code_ptr, REG_A4, seg, insn_eip);
                        last_cmp_type = CMP_NONE;
                    } else {
                        goto fallback_exit;
                    }
                } else {
                    /* Read-only ALU with a memory source operand. Load r/m32
                     * into REG_T5 (which emit_flags_* do not clobber, unlike
                     * T0/T2/T3/T4), then compute into the destination register.
                     * CMP/TEST materialise flags here and clear last_cmp_type,
                     * so the mem temp never has to survive past this instruction. */
                    emit_mem_load32(&code_ptr, REG_T5, seg, insn_eip);
                    switch (op) {
                    case 0x03: /* ADD r32, r/m32 */
                        emit_add(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_flags_add(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                        break;
                    case 0x2B: /* SUB r32, r/m32 */
                        emit_sub(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                        break;
                    case 0x23: /* AND r32, r/m32 */
                        emit_and(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_flags_logic(&code_ptr, REG_T1);
                        emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                        break;
                    case 0x0B: /* OR r32, r/m32 */
                        emit_or(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_flags_logic(&code_ptr, REG_T1);
                        emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                        break;
                    case 0x33: /* XOR r32, r/m32 */
                        emit_xor(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_flags_logic(&code_ptr, REG_T1);
                        emit_addi(&code_ptr, x86_to_rv[reg], REG_T1, 0);
                        break;
                    case 0x3B: /* CMP r32, r/m32 (read-only) */
                        emit_sub(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        emit_flags_sub(&code_ptr, REG_T1, x86_to_rv[reg], REG_T5);
                        break;
                    case 0x39: /* CMP r/m32, r32 (read-only) */
                        emit_sub(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_sub(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        break;
                    case 0x85: /* TEST r/m32, r32 (read-only) */
                        emit_and(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg]);
                        emit_flags_logic(&code_ptr, REG_T1);
                        break;
                    }
                    last_cmp_type = CMP_NONE;
                }

                cur_eip += insn_len;
                insn_count++;
                continue;
            }
        }

        /* Control Flow Terminators */
        /* JMP rel8 (0xEB) */
        if (op == 0xEB) {
            int8_t rel8 = (int8_t)jit_read_u8(cpu, cur_eip + 1);
            uint32_t target = cur_eip + 2 + rel8;
            cur_eip = target;
            insn_count++;
            goto block_done;
        }

        /* JMP rel32 (0xE9) */
        if (op == 0xE9) {
            int32_t rel32 = (int32_t)jit_read_u32(cpu, cur_eip + 1);
            uint32_t target = cur_eip + 5 + rel32;
            cur_eip = target;
            insn_count++;
            goto block_done;
        }

        /* 1-byte Opcode Conditional Jumps: 0x70 .. 0x7F */
        if (op >= 0x70 && op <= 0x7F) {
            int8_t rel8 = (int8_t)jit_read_u8(cpu, cur_eip + 1);
            uint32_t target_eip = cur_eip + 2 + rel8;
            uint32_t next_eip   = cur_eip + 2;

            if (op == 0x74 || op == 0x75 ||
                ((op == 0x72 || op == 0x73 || op == 0x76 || op == 0x77) &&
                 (prev_cmp == CMP_REG_REG || prev_cmp == CMP_REG_IMM))) {
                
                if (prev_cmp == CMP_NONE) {
                    emit_lw(&code_ptr, REG_T1, REG_S0, 40);
                    emit_andi(&code_ptr, REG_T1, REG_T1, 0x40); /* test ZF bit 6 */
                } else if (prev_cmp == CMP_REG_IMM) {
                    emit_mov_imm32(&code_ptr, REG_T0, cmp_imm);
                }

                uint32_t *branch_insn = code_ptr;
                emit_beq(&code_ptr, REG_T1, REG_ZERO, 0); /* placeholder */
                emit_block_tail(&code_ptr, next_eip, (uint32_t)(insn_count + 1),
                                cpu, blk_start_off, start_eip);
                uint32_t *taken_target = code_ptr;
                int32_t offset = (int32_t)((uint8_t *)taken_target - (uint8_t *)branch_insn);

                uint32_t *p = branch_insn;
                switch (op) {
                case 0x74: /* JZ / JE */
                    if (prev_cmp == CMP_NONE) emit_bne(&p, REG_T1, REG_ZERO, offset);
                    else emit_beq(&p, REG_T1, REG_ZERO, offset);
                    break;
                case 0x75: /* JNZ / JNE */
                    if (prev_cmp == CMP_NONE) emit_beq(&p, REG_T1, REG_ZERO, offset);
                    else emit_bne(&p, REG_T1, REG_ZERO, offset);
                    break;
                case 0x72: /* JB / JC (unsigned <) */
                    if (prev_cmp == CMP_REG_REG) emit_bltu(&p, cmp_r1, cmp_r2, offset);
                    else emit_bltu(&p, cmp_r1, REG_T0, offset);
                    break;
                case 0x73: /* JAE / JNC (unsigned >=) */
                    if (prev_cmp == CMP_REG_REG) emit_bgeu(&p, cmp_r1, cmp_r2, offset);
                    else emit_bgeu(&p, cmp_r1, REG_T0, offset);
                    break;
                case 0x77: /* JA / JNBE (unsigned >) */
                    if (prev_cmp == CMP_REG_REG) emit_bltu(&p, cmp_r2, cmp_r1, offset);
                    else emit_bltu(&p, REG_T0, cmp_r1, offset);
                    break;
                case 0x76: /* JBE / JNA (unsigned <=) */
                    if (prev_cmp == CMP_REG_REG) emit_bgeu(&p, cmp_r2, cmp_r1, offset);
                    else emit_bgeu(&p, REG_T0, cmp_r1, offset);
                    break;
                }

                emit_block_tail(&code_ptr, target_eip, (uint32_t)(insn_count + 1),
                                cpu, blk_start_off, start_eip);
                insn_count++;
                goto block_finished;
            }
        }

        /* 2-byte Opcode: 0x0F */
        if (op == 0x0F) {
            uint8_t op2 = jit_read_u8(cpu, cur_eip + 1);

            /* IMUL r32, r/m32 (0x0F 0xAF) */
            if (op2 == 0xAF) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_mul(&code_ptr, x86_to_rv[reg2], x86_to_rv[reg2], x86_to_rv[rm2]);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* MOVZX r32, r/m8 (0x0F 0xB6) */
            if (op2 == 0xB6) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    if (rm2 < 4) {
                        emit_andi(&code_ptr, x86_to_rv[reg2], x86_to_rv[rm2], 0xFF);
                    } else {
                        emit_srli(&code_ptr, REG_T1, x86_to_rv[rm2 - 4], 8);
                        emit_andi(&code_ptr, x86_to_rv[reg2], REG_T1, 0xFF);
                    }
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* MOVZX r32, r/m16 (0x0F 0xB7) */
            if (op2 == 0xB7) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_slli(&code_ptr, REG_T1, x86_to_rv[rm2], 16);
                    emit_srli(&code_ptr, x86_to_rv[reg2], REG_T1, 16);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* MOVSX r32, r/m8 (0x0F 0xBE) */
            if (op2 == 0xBE) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    if (rm2 < 4) {
                        emit_slli(&code_ptr, REG_T1, x86_to_rv[rm2], 24);
                        emit_srai(&code_ptr, x86_to_rv[reg2], REG_T1, 24);
                    } else {
                        emit_slli(&code_ptr, REG_T1, x86_to_rv[rm2 - 4], 16);
                        emit_srai(&code_ptr, x86_to_rv[reg2], REG_T1, 24);
                    }
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* MOVSX r32, r/m16 (0x0F 0xBF) */
            if (op2 == 0xBF) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_slli(&code_ptr, REG_T1, x86_to_rv[rm2], 16);
                    emit_srai(&code_ptr, x86_to_rv[reg2], REG_T1, 16);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BSWAP r32 (0x0F 0xC8 .. 0x0F 0xCF) - RV32 Zbb rev8 */
            if (op2 >= 0xC8 && op2 <= 0xCF) {
                int rd = op2 - 0xC8;
                emit_rev8(&code_ptr, x86_to_rv[rd], x86_to_rv[rd]);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }

            /* BSF r32, r/m32 (0x0F 0xBC) - RV32 Zbb ctz */
            if (op2 == 0xBC) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_ctz(&code_ptr, x86_to_rv[reg2], x86_to_rv[rm2]);
                    emit_sltiu(&code_ptr, REG_T1, x86_to_rv[rm2], 1); /* ZF = (src == 0) */
                    emit_flags_zf(&code_ptr, REG_T1);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BSR r32, r/m32 (0x0F 0xBD) - RV32 Zbb clz */
            if (op2 == 0xBD) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_clz(&code_ptr, REG_T1, x86_to_rv[rm2]);
                    emit_addi(&code_ptr, REG_T0, REG_ZERO, 31);
                    emit_sub(&code_ptr, x86_to_rv[reg2], REG_T0, REG_T1);
                    emit_sltiu(&code_ptr, REG_T1, x86_to_rv[rm2], 1); /* ZF = (src == 0) */
                    emit_flags_zf(&code_ptr, REG_T1);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BT r/m32, r32 (0x0F 0xA3) - RV32 Zbs bext */
            if (op2 == 0xA3) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_bext(&code_ptr, REG_T1, x86_to_rv[rm2], x86_to_rv[reg2]);
                    emit_flags_cf(&code_ptr, REG_T1);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BTS r/m32, r32 (0x0F 0xAB) - RV32 Zbs bext + bset */
            if (op2 == 0xAB) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_bext(&code_ptr, REG_T1, x86_to_rv[rm2], x86_to_rv[reg2]);
                    emit_flags_cf(&code_ptr, REG_T1);
                    emit_bset(&code_ptr, x86_to_rv[rm2], x86_to_rv[rm2], x86_to_rv[reg2]);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BTR r/m32, r32 (0x0F 0xB3) - RV32 Zbs bext + bclr */
            if (op2 == 0xB3) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_bext(&code_ptr, REG_T1, x86_to_rv[rm2], x86_to_rv[reg2]);
                    emit_flags_cf(&code_ptr, REG_T1);
                    emit_bclr(&code_ptr, x86_to_rv[rm2], x86_to_rv[rm2], x86_to_rv[reg2]);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BTC r/m32, r32 (0x0F 0xBB) - RV32 Zbs bext + binv */
            if (op2 == 0xBB) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_bext(&code_ptr, REG_T1, x86_to_rv[rm2], x86_to_rv[reg2]);
                    emit_flags_cf(&code_ptr, REG_T1);
                    emit_binv(&code_ptr, x86_to_rv[rm2], x86_to_rv[rm2], x86_to_rv[reg2]);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* Group 8: BT/BTS/BTR/BTC r/m32, imm8 (0x0F 0xBA) */
            if (op2 == 0xBA) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t subop = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3 && subop >= 4) {
                    uint8_t imm8 = jit_read_u8(cpu, cur_eip + 3) & 31;
                    emit_bexti(&code_ptr, REG_T1, x86_to_rv[rm2], imm8);
                    emit_flags_cf(&code_ptr, REG_T1);
                    if (subop == 5) {
                        emit_bseti(&code_ptr, x86_to_rv[rm2], x86_to_rv[rm2], imm8);
                    } else if (subop == 6) {
                        emit_bclri(&code_ptr, x86_to_rv[rm2], x86_to_rv[rm2], imm8);
                    } else if (subop == 7) {
                        emit_binvi(&code_ptr, x86_to_rv[rm2], x86_to_rv[rm2], imm8);
                    }
                    cur_eip += 4;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* SETcc r8 (0x0F 0x90 .. 0x0F 0x9F) */
            if (op2 >= 0x90 && op2 <= 0x9F) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    uint8_t cc = op2 & 0x0F;
                    /* Load cpu->flags (offset 40) into REG_T4 */
                    emit_lw(&code_ptr, REG_T4, REG_S0, 40);
                    /* Extract condition flags */
                    emit_andi(&code_ptr, REG_T0, REG_T4, 1);       /* CF (bit 0) */
                    emit_srli(&code_ptr, REG_T1, REG_T4, 6);
                    emit_andi(&code_ptr, REG_T1, REG_T1, 1);       /* ZF (bit 6) */
                    emit_srli(&code_ptr, REG_T2, REG_T4, 7);
                    emit_andi(&code_ptr, REG_T2, REG_T2, 1);       /* SF (bit 7) */
                    emit_srli(&code_ptr, REG_T3, REG_T4, 11);
                    emit_andi(&code_ptr, REG_T3, REG_T3, 1);       /* OF (bit 11) */

                    switch (cc) {
                    case 0x0: /* SETO (OF == 1) */
                        emit_addi(&code_ptr, REG_T5, REG_T3, 0);
                        break;
                    case 0x1: /* SETNO (OF == 0) */
                        emit_xori(&code_ptr, REG_T5, REG_T3, 1);
                        break;
                    case 0x2: /* SETB / SETC (CF == 1) */
                        emit_addi(&code_ptr, REG_T5, REG_T0, 0);
                        break;
                    case 0x3: /* SETAE / SETNC (CF == 0) */
                        emit_xori(&code_ptr, REG_T5, REG_T0, 1);
                        break;
                    case 0x4: /* SETZ / SETE (ZF == 1) */
                        emit_addi(&code_ptr, REG_T5, REG_T1, 0);
                        break;
                    case 0x5: /* SETNZ / SETNE (ZF == 0) */
                        emit_xori(&code_ptr, REG_T5, REG_T1, 1);
                        break;
                    case 0x6: /* SETBE (CF == 1 || ZF == 1) */
                        emit_or(&code_ptr, REG_T5, REG_T0, REG_T1);
                        break;
                    case 0x7: /* SETA (CF == 0 && ZF == 0) */
                        emit_or(&code_ptr, REG_T5, REG_T0, REG_T1);
                        emit_xori(&code_ptr, REG_T5, REG_T5, 1);
                        break;
                    case 0x8: /* SETS (SF == 1) */
                        emit_addi(&code_ptr, REG_T5, REG_T2, 0);
                        break;
                    case 0x9: /* SETNS (SF == 0) */
                        emit_xori(&code_ptr, REG_T5, REG_T2, 1);
                        break;
                    case 0xC: /* SETL (SF != OF) */
                        emit_xor(&code_ptr, REG_T5, REG_T2, REG_T3);
                        break;
                    case 0xD: /* SETGE (SF == OF) */
                        emit_xor(&code_ptr, REG_T5, REG_T2, REG_T3);
                        emit_xori(&code_ptr, REG_T5, REG_T5, 1);
                        break;
                    case 0xE: /* SETLE (ZF == 1 || SF != OF) */
                        emit_xor(&code_ptr, REG_T5, REG_T2, REG_T3);
                        emit_or(&code_ptr, REG_T5, REG_T5, REG_T1);
                        break;
                    case 0xF: /* SETG (ZF == 0 && SF == OF) */
                        emit_xor(&code_ptr, REG_T5, REG_T2, REG_T3);
                        emit_or(&code_ptr, REG_T5, REG_T5, REG_T1);
                        emit_xori(&code_ptr, REG_T5, REG_T5, 1);
                        break;
                    default:
                        goto fallback_exit;
                    }

                    emit_write_r8(&code_ptr, rm2, REG_T5);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* SHLD r/m32, r32, imm8 (0x0F 0xA4) / SHLD r/m32, r32, CL (0x0F 0xA5)
             * SHRD r/m32, r32, imm8 (0x0F 0xAC) / SHRD r/m32, r32, CL (0x0F 0xAD)
             * Register (mod==3) forms only; memory forms fall back to the
             * interpreter. 486-era core opcodes CPU-Z and bitmap code hammer. */
            if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                int is_shld = (op2 == 0xA4 || op2 == 0xA5);
                int is_imm  = (op2 == 0xA4 || op2 == 0xAC);
                if (mod2 == 3) {
                    if (is_imm) {
                        uint8_t cnt = jit_read_u8(cpu, cur_eip + 3) & 0x1f;
                        if (cnt == 0) {
                            /* count 0: dst and every flag stay unchanged */
                            cur_eip += 4;
                            insn_count++;
                            last_cmp_type = CMP_NONE;
                            continue;
                        }
                        emit_mov_imm32(&code_ptr, REG_T6, cnt);
                    } else {
                        /* CL form: mask to 5 bits first (a dst==ECX encoding
                         * must not corrupt the count), then skip if count==0 */
                        emit_andi(&code_ptr, REG_T6, x86_to_rv[1], 0x1f);
                        uint32_t *br = code_ptr;
                        emit_beq(&code_ptr, REG_T6, REG_ZERO, 0);
                        emit_shxd(&code_ptr, x86_to_rv[rm2], x86_to_rv[reg2], REG_T6, is_shld);
                        emit_beq(&br, REG_T6, REG_ZERO,
                                 (int32_t)((uint8_t *)code_ptr - (uint8_t *)br));
                        cur_eip += 3;
                        insn_count++;
                        last_cmp_type = CMP_NONE;
                        continue;
                    }
                    emit_shxd(&code_ptr, x86_to_rv[rm2], x86_to_rv[reg2], REG_T6, is_shld);
                    cur_eip += 4;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* XADD r/m32, r32 (0x0F 0xC1) - 486 */
            if (op2 == 0xC1) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    /* old a -> T5, res -> T1, flags first (src still holds old
                     * b), then write b = old a and a = res so a==b yields res. */
                    emit_addi(&code_ptr, REG_T5, x86_to_rv[rm2], 0);
                    emit_add(&code_ptr, REG_T1, x86_to_rv[rm2], x86_to_rv[reg2]);
                    emit_flags_add(&code_ptr, REG_T1, REG_T5, x86_to_rv[reg2]);
                    emit_addi(&code_ptr, x86_to_rv[reg2], REG_T5, 0);
                    emit_addi(&code_ptr, x86_to_rv[rm2], REG_T1, 0);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* CMPXCHG r/m32, r32 (0x0F 0xB1) - 486.
             * res = r/m - EAX; if res==0 store src into r/m, else EAX = r/m. */
            if (op2 == 0xB1) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_addi(&code_ptr, REG_T5, x86_to_rv[rm2], 0);       /* src1 */
                    emit_sub(&code_ptr, REG_T1, REG_T5, x86_to_rv[0]);     /* res */
                    emit_flags_sub(&code_ptr, REG_T1, REG_T5, x86_to_rv[0]);
                    uint32_t *br = code_ptr;
                    emit_bne(&code_ptr, REG_T1, REG_ZERO, 0);              /* res != 0 -> else */
                    emit_addi(&code_ptr, x86_to_rv[rm2], x86_to_rv[reg2], 0); /* equal: r/m = src */
                    uint32_t *jmpp = code_ptr;
                    emit_jal(&code_ptr, REG_ZERO, 0);
                    uint32_t *else_start = code_ptr;
                    emit_addi(&code_ptr, x86_to_rv[0], REG_T5, 0);         /* else: EAX = src1 */
                    emit_bne(&br, REG_T1, REG_ZERO,
                             (int32_t)((uint8_t *)else_start - (uint8_t *)br));
                    emit_jal(&jmpp, REG_ZERO, (int32_t)((uint8_t *)code_ptr - (uint8_t *)jmpp));
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* 2-byte Near Conditional Jumps: 0x0F 0x80 .. 0x0F 0x8F */
            if (op2 >= 0x80 && op2 <= 0x8F) {
                int32_t rel32 = (int32_t)jit_read_u32(cpu, cur_eip + 2);
                uint32_t target_eip = cur_eip + 6 + rel32;
                uint32_t next_eip   = cur_eip + 6;

                if (op2 == 0x84 || op2 == 0x85 ||
                    ((op2 == 0x82 || op2 == 0x83 || op2 == 0x86 || op2 == 0x87) &&
                     (prev_cmp == CMP_REG_REG || prev_cmp == CMP_REG_IMM))) {
                    
                    if (prev_cmp == CMP_NONE) {
                        emit_lw(&code_ptr, REG_T1, REG_S0, 40);
                        emit_andi(&code_ptr, REG_T1, REG_T1, 0x40); /* test ZF bit 6 */
                    } else if (prev_cmp == CMP_REG_IMM) {
                        emit_mov_imm32(&code_ptr, REG_T0, cmp_imm);
                    }

                    uint32_t *branch_insn = code_ptr;
                    emit_beq(&code_ptr, REG_T1, REG_ZERO, 0); /* placeholder */
                    emit_block_tail(&code_ptr, next_eip, (uint32_t)(insn_count + 1),
                                    cpu, blk_start_off, start_eip);
                    uint32_t *taken_target = code_ptr;
                    int32_t offset = (int32_t)((uint8_t *)taken_target - (uint8_t *)branch_insn);

                    uint32_t *p = branch_insn;
                    switch (op2) {
                    case 0x84: /* JZ rel32 */
                        if (prev_cmp == CMP_NONE) emit_bne(&p, REG_T1, REG_ZERO, offset);
                        else emit_beq(&p, REG_T1, REG_ZERO, offset);
                        break;
                    case 0x85: /* JNZ rel32 */
                        if (prev_cmp == CMP_NONE) emit_beq(&p, REG_T1, REG_ZERO, offset);
                        else emit_bne(&p, REG_T1, REG_ZERO, offset);
                        break;
                    case 0x82: /* JB rel32 */
                        if (prev_cmp == CMP_REG_REG) emit_bltu(&p, cmp_r1, cmp_r2, offset);
                        else emit_bltu(&p, cmp_r1, REG_T0, offset);
                        break;
                    case 0x83: /* JAE rel32 */
                        if (prev_cmp == CMP_REG_REG) emit_bgeu(&p, cmp_r1, cmp_r2, offset);
                        else emit_bgeu(&p, cmp_r1, REG_T0, offset);
                        break;
                    case 0x87: /* JA rel32 */
                        if (prev_cmp == CMP_REG_REG) emit_bltu(&p, cmp_r2, cmp_r1, offset);
                        else emit_bltu(&p, REG_T0, cmp_r1, offset);
                        break;
                    case 0x86: /* JBE rel32 */
                        if (prev_cmp == CMP_REG_REG) emit_bgeu(&p, cmp_r2, cmp_r1, offset);
                        else emit_bgeu(&p, REG_T0, cmp_r1, offset);
                        break;
                    }

                    emit_block_tail(&code_ptr, target_eip, (uint32_t)(insn_count + 1),
                                    cpu, blk_start_off, start_eip);
                    insn_count++;
                    last_op = op2;
                    goto block_finished;
                }
            }
        }

    fallback_exit:
        break;
    }

    /* Block did not end in a recognized terminator (unsupported instruction or
       max_insns reached). If we already translated at least one instruction,
       compile the supported prefix and fall through to the next EIP so the
       interpreter handles the remaining instruction(s). Only mark the block as
       permanently un-compilable when nothing could be translated at all. */
    if (insn_count > 0) {
        goto block_done;
    }
    { bool ok; uint32_t key = jit_key_of(cpu, start_eip, &ok); (void)key;
      if (ok) jit_insert_block(cpu, start_eip, NULL, 0); }
    return NULL;

block_done:
    emit_block_tail(&code_ptr, cur_eip, (uint32_t)insn_count,
                    cpu, blk_start_off, start_eip);

block_finished:
    size_t block_bytes = (uint8_t *)code_ptr - (uint8_t *)code_start;
    s_jit_off += (block_bytes + 15) & ~15;

    jit_sync_range(code_start, block_bytes);

    { bool ok; uint32_t key = jit_key_of(cpu, start_eip, &ok); (void)key;
      if (ok) { jit_mark_code_pages(cpu, start_eip, cur_eip > start_eip ? cur_eip : start_eip + 1);
                jit_insert_block(cpu, start_eip, (void *)code_start, (insn_count > 0) ? insn_count : 1); } }
#ifdef DEBUG_JIT_COMPILE
    fprintf(stderr, "[JIT COMPILE #%u] pc=%08x..%08x (len=%d insns)\n", s_jit_compile_count, start_eip, cur_eip, insn_count);
#endif
    s_jit_compile_count++;

    return (void *)code_start;
}
