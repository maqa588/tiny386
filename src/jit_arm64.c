#include "jit_arm64.h"
#include "i386_struct.h"
#include "jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#endif

/*
 * ============================================================================
 * Tiny386 AArch64 (ARMv8 / Apple Silicon) Dynamic Binary Translator (JIT)
 * ============================================================================
 */

/* --- AArch64 Registers --- */
#define REG_W0   0
#define REG_W1   1
#define REG_W2   2
#define REG_W3   3
#define REG_W9   9
#define REG_W10  10
#define REG_W11  11
#define REG_W12  12
#define REG_W13  13
#define REG_W14  14
#define REG_W15  15
#define REG_W16  16
#define REG_W19  19
#define REG_W20  20
#define REG_W21  21
#define REG_W22  22
#define REG_W23  23
#define REG_W24  24
#define REG_W25  25
#define REG_W26  26
#define REG_W27  27
#define REG_W28  28
#define REG_W29  29
#define REG_W30  30
#define REG_WZR  31
#define REG_SP   31

#define REG_X0   0
#define REG_X1   1
#define REG_X2   2
#define REG_X3   3
#define REG_X9   9
#define REG_X10  10
#define REG_X11  11
#define REG_X12  12
#define REG_X13  13
#define REG_X14  14
#define REG_X15  15
#define REG_X16  16
#define REG_X19  19
#define REG_X20  20
#define REG_X21  21
#define REG_X22  22
#define REG_X23  23
#define REG_X24  24
#define REG_X25  25
#define REG_X26  26
#define REG_X27  27
#define REG_X28  28
#define REG_X29  29
#define REG_X30  30
#define REG_XZR  31

/* Map x86 32-bit GPR index (0..7: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI) to ARM64 register */
static const int x86_to_arm[8] = {
    REG_W20, /* 0: EAX */
    REG_W21, /* 1: ECX */
    REG_W22, /* 2: EDX */
    REG_W23, /* 3: EBX */
    REG_W24, /* 4: ESP */
    REG_W25, /* 5: EBP */
    REG_W26, /* 6: ESI */
    REG_W27  /* 7: EDI */
};

/* Condition codes */
enum {
    COND_EQ = 0x0,
    COND_NE = 0x1,
    COND_CS = 0x2, COND_HS = 0x2,
    COND_CC = 0x3, COND_LO = 0x3,
    COND_MI = 0x4,
    COND_PL = 0x5,
    COND_VS = 0x6,
    COND_VC = 0x7,
    COND_HI = 0x8,
    COND_LS = 0x9,
    COND_GE = 0xA,
    COND_LT = 0xB,
    COND_GT = 0xC,
    COND_LE = 0xD,
    COND_AL = 0xE
};

#define CPU_OFF_GPR(i)   ((uint32_t)offsetof(CPUI386, gprx[i].r32))
#define CPU_OFF_IP       ((uint32_t)offsetof(CPUI386, ip))
#define CPU_OFF_NEXT_IP  ((uint32_t)offsetof(CPUI386, next_ip))
#define CPU_OFF_FLAGS    ((uint32_t)offsetof(CPUI386, flags))
#define CPU_OFF_CC_MASK  ((uint32_t)offsetof(CPUI386, cc.mask))
#define CPU_OFF_INTR     ((uint32_t)offsetof(CPUI386, intr))
#define CPU_OFF_CYCLE    ((uint32_t)offsetof(CPUI386, cycle))
/* seg[i].base sits 4 bytes into each 16-byte segment descriptor */
#define CPU_OFF_SEG_BASE(i) ((uint32_t)(offsetof(CPUI386, seg) + 4 + 16 * (i)))

/* Compiled-block frame layout (JIT_FRAME_SIZE bytes, 16-byte aligned):
 *   [sp,#0]   saved x29/x30        [sp,#16..87] saved x19..x28
 *   [sp,#96]  temporary 32-bit buffer for guest memory loads
 *   [sp,#100] EA spill for read-modify-write memory ops (survives cpu_* calls)
 *   [sp,#104] cached `jit_fast_tlb` base (see emit_prologue)
 *   [sp,#112] cached address of the chain guard stamp (see emit_prologue)
 *   [sp,#120] cached `jit_page_gen` array base (see emit_prologue)
 * x19 = CPUI386*, x20..x27 = x86 EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI,
 * x28 = absolute cpu->cycle deadline for the current chain session. */
#define JIT_FRAME_SIZE      128
#define JIT_SLOT_LOAD       96
#define JIT_SLOT_EA         100
#define JIT_SLOT_TLB        104
#define JIT_SLOT_GUARD      112
#define JIT_SLOT_PAGEGEN    120
/* emit_prologue() emits exactly this many instructions; chain slots jump
 * straight to a successor's body, i.e. to code_off + JIT_PROLOGUE_BYTES. */
#define JIT_PROLOGUE_BYTES  152
#define JIT_NO_CHAIN        0xFFFFFFFFu

#define JIT_BUF_SIZE        (16 * 1024 * 1024)  /* 16MB executable buffer */
#define JIT_HASH_SETS       16384               /* 16K sets */
#define JIT_HASH_WAYS       2                   /* 2-way set associative */
#define JIT_HOTSPOT_SIZE    8192                /* 8K hotspot counters */
#define JIT_HOTSPOT_THRESHOLD 64                /* Trigger compile on 64 executions of the exact PC */

typedef struct {
    uint32_t guest_pc;
    uint32_t phys_and_insns; /* high 20 bits: physical page number of the code page; low 12 bits: insn_count (always >= 1) */
    uint32_t code_off;       /* byte offset of the block within s_jit_buf */
} JITEntry;

static uint8_t *s_jit_buf = NULL;
static size_t s_jit_off = 0;
static JITEntry (*s_hash_tab)[JIT_HASH_WAYS] = NULL;
static uint8_t *s_hash_lru = NULL;
/* Hotspot table: direct-mapped, tag-validated per-guest-PC execution counters.
 * Storing the exact guest_pc (tag) alongside the count prevents unrelated PCs
 * that alias the same slot from spuriously inflating each other's count, which
 * caused the compile threshold to fire for cold code. */
static uint32_t *s_hotspot_tag = NULL;
static uint8_t *s_hotspot_cnt = NULL;
#define JIT_HOTSPOT_SHIFT 13 /* log2(JIT_HOTSPOT_SIZE) */

/* Transition cache: direct-mapped memo from a guest successor PC to the block
 * that should run next. It lets the dispatch loop skip the VA->PA TLB probe and
 * the set-associative hash walk on the common fall-through/jump transition.
 * Validity is checked against the mapping epoch (bumped on any TLB clear) and
 * the physical page generation (bumped on self-modifying-code writes), so a
 * cached entry is reused only while its page mapping and code bytes are intact. */
#define JIT_XLATE_SHIFT 12 /* 4096 direct-mapped entries */
typedef struct {
    uint32_t pc;       /* guest PC; validity tag (entry live iff epoch != 0) */
    uint32_t code_off; /* block offset in s_jit_buf */
    uint32_t key;      /* physical page key at fill time */
    uint32_t epoch;    /* mapping epoch at fill time; 0 = invalid */
    uint32_t insns;    /* block instruction count */
    uint8_t  gen;      /* page generation at fill time */
} JITXlate;
static JITXlate *s_xlate = NULL;
static uint32_t s_xlate_epoch = 1; /* never 0; bumped by jit_note_map_change() */
static bool s_initialized = false;
static bool s_jit_enabled = true;
static int s_jit_ring0_allowed = 0;
static uint32_t s_jit_exec_count = 0;
static uint32_t s_jit_compile_count = 0;

uint8_t *jit_smc_page_map = NULL;
uint8_t *jit_page_gen = NULL;
static uint8_t *s_entry_gen = NULL; /* [JIT_HASH_SETS][JIT_HASH_WAYS]: page gen recorded at insert */
volatile uint8_t jit_smc_dirty = 0;

static inline uint32_t jit_pack_tag(uint32_t phys, uint32_t insns) {
    return ((phys & 0xFFFFFu) << 12) | (insns & 0xFFFu);
}

static inline void *jit_entry_fn(const JITEntry *e) {
    return (void *)(s_jit_buf + e->code_off);
}

/* External helpers declared in i386.h */
extern bool cpu_load8(CPUI386 *cpu, int seg, uword addr, uint8_t *res);
extern bool cpu_load32(CPUI386 *cpu, int seg, uword addr, uint32_t *res);
extern bool cpu_store32(CPUI386 *cpu, int seg, uword addr, uint32_t val);
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

#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

/*
 * macOS W^X (Write XOR Execute) support:
 * On Apple Silicon, JIT pages allocated with MAP_JIT require pthread_jit_write_protect_np(0)
 * before writing instructions, and pthread_jit_write_protect_np(1) before execution.
 */
static inline void jit_write_enable(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0);
#endif
}

static inline void jit_write_disable(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(1);
#endif
}

static inline void jit_sync_range(void *addr, size_t size) {
    if (addr && size > 0) {
        __builtin___clear_cache((char *)addr, (char *)addr + size);
    }
}

void jit_flush_cache(void) {
    if (s_jit_buf && s_jit_off > 0) {
        __builtin___clear_cache((char *)s_jit_buf, (char *)s_jit_buf + s_jit_off);
    }
}

/* --- Instruction Emission Helpers --- */

static inline void emit32(uint32_t **buf, uint32_t val) {
    **buf = val;
    (*buf)++;
}

static inline void emit_add_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x0B000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_add_x(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x8B000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_add_shift_w(uint32_t **buf, int rd, int rn, int rm, int shift) {
    emit32(buf, 0x0B000000 | ((shift & 31) << 10) | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_sub_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x4B000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_adds_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x2B000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_subs_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x6B000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_cmp_w(uint32_t **buf, int rn, int rm) {
    emit_subs_w(buf, REG_WZR, rn, rm);
}

static inline __attribute__((unused)) void emit_neg_w(uint32_t **buf, int rd, int rm) {
    emit_sub_w(buf, rd, REG_WZR, rm);
}

static inline void emit_add_imm_w(uint32_t **buf, int rd, int rn, uint32_t imm12) {
    emit32(buf, 0x11000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static inline void emit_sub_imm_w(uint32_t **buf, int rd, int rn, uint32_t imm12) {
    emit32(buf, 0x51000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static inline void emit_cmp_imm_w(uint32_t **buf, int rn, uint32_t imm12) {
    emit32(buf, 0x7100001F | ((imm12 & 0xFFF) << 10) | (rn << 5));
}

static inline void emit_add_imm_x(uint32_t **buf, int rd, int rn, uint32_t imm12) {
    emit32(buf, 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_sub_imm_x(uint32_t **buf, int rd, int rn, uint32_t imm12) {
    emit32(buf, 0xD1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static inline void emit_mov_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x2A0003E0 | (rn << 16) | rd);
}

static inline void emit_mov_x(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0xAA0003E0 | (rn << 16) | rd);
}

static inline void emit_and_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x0A000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_orr_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x2A000000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_orr_shift_w(uint32_t **buf, int rd, int rn, int rm, int shift) {
    emit32(buf, 0x2A000000 | ((shift & 31) << 10) | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_eor_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x4A000000 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_orn_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x2AA00000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_mvn_w(uint32_t **buf, int rd, int rm) {
    emit32(buf, 0x2AA003E0 | (rm << 16) | rd);
}

static inline void emit_bic_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x0AA00000 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_tst_w(uint32_t **buf, int rn, int rm) {
    emit32(buf, 0x6A00001F | (rm << 16) | (rn << 5));
}

static inline void emit_mul_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1B007C00 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_sdiv_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1AC00C00 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_udiv_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1AC00800 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_msub_w(uint32_t **buf, int rd, int rn, int rm, int ra) {
    emit32(buf, 0x1B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

static inline void emit_lslv_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1AC02000 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_lsrv_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1AC02400 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_asrv_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1AC02800 | (rm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_rorv_w(uint32_t **buf, int rd, int rn, int rm) {
    emit32(buf, 0x1AC02C00 | (rm << 16) | (rn << 5) | rd);
}

static inline void emit_lsl_imm_w(uint32_t **buf, int rd, int rn, uint32_t imm) {
    imm &= 31;
    if (imm == 0) {
        emit_mov_w(buf, rd, rn);
    } else {
        emit32(buf, 0x53000000 | (((32 - imm) & 31) << 16) | ((31 - imm) << 10) | (rn << 5) | rd);
    }
}

static inline __attribute__((unused)) void emit_lsr_imm_w(uint32_t **buf, int rd, int rn, uint32_t imm) {
    imm &= 31;
    emit32(buf, 0x53007C00 | (imm << 16) | (rn << 5) | rd);
}

static inline void emit_asr_imm_w(uint32_t **buf, int rd, int rn, uint32_t imm) {
    imm &= 31;
    emit32(buf, 0x13007C00 | (imm << 16) | (rn << 5) | rd);
}

static inline __attribute__((unused)) void emit_ror_imm_w(uint32_t **buf, int rd, int rn, uint32_t imm) {
    imm &= 31;
    emit32(buf, 0x13800000 | (rn << 16) | (imm << 10) | (rn << 5) | rd);
}

static inline void emit_rev_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x5AC00800 | (rn << 5) | rd);
}

static inline void emit_clz_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x5AC01000 | (rn << 5) | rd);
}

static inline void emit_rbit_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x5AC00000 | (rn << 5) | rd);
}

static inline void emit_ctz_w(uint32_t **buf, int rd, int rn) {
    emit_rbit_w(buf, rd, rn);
    emit_clz_w(buf, rd, rd);
}

static inline void emit_uxtb_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x53001C00 | (rn << 5) | rd);
}

static inline void emit_uxth_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x53003C00 | (rn << 5) | rd);
}

static inline void emit_sxtb_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x13001C00 | (rn << 5) | rd);
}

static inline void emit_sxth_w(uint32_t **buf, int rd, int rn) {
    emit32(buf, 0x13003C00 | (rn << 5) | rd);
}

static inline void emit_ubfx_w(uint32_t **buf, int rd, int rn, uint32_t lsb, uint32_t width) {
    emit32(buf, 0x53000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd);
}

static inline void emit_sbfx_w(uint32_t **buf, int rd, int rn, uint32_t lsb, uint32_t width) {
    emit32(buf, 0x13000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd);
}

static inline void emit_bfi_w(uint32_t **buf, int rd, int rn, uint32_t lsb, uint32_t width) {
    emit32(buf, 0x33000000 | (((32 - lsb) & 31) << 16) | ((width - 1) << 10) | (rn << 5) | rd);
}

static inline void emit_movz_w(uint32_t **buf, int rd, uint32_t imm16, int hw) {
    emit32(buf, 0x52800000 | ((hw & 1) << 21) | ((imm16 & 0xFFFF) << 5) | rd);
}

static inline void emit_movk_w(uint32_t **buf, int rd, uint32_t imm16, int hw) {
    emit32(buf, 0x72800000 | ((hw & 1) << 21) | ((imm16 & 0xFFFF) << 5) | rd);
}

static inline void emit_movz_x(uint32_t **buf, int rd, uint32_t imm16, int hw) {
    emit32(buf, 0xD2800000 | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | rd);
}

static inline void emit_movk_x(uint32_t **buf, int rd, uint32_t imm16, int hw) {
    emit32(buf, 0xF2800000 | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | rd);
}

static void emit_mov_imm32(uint32_t **buf, int rd, uint32_t imm32) {
    if ((imm32 >> 16) == 0) {
        emit_movz_w(buf, rd, imm32 & 0xFFFF, 0);
    } else if ((imm32 & 0xFFFF) == 0) {
        emit_movz_w(buf, rd, (imm32 >> 16) & 0xFFFF, 1);
    } else {
        emit_movz_w(buf, rd, imm32 & 0xFFFF, 0);
        emit_movk_w(buf, rd, (imm32 >> 16) & 0xFFFF, 1);
    }
}

static void emit_mov_imm64(uint32_t **buf, int rd, uint64_t imm64) {
    uint16_t h0 = imm64 & 0xFFFF;
    uint16_t h1 = (imm64 >> 16) & 0xFFFF;
    uint16_t h2 = (imm64 >> 32) & 0xFFFF;
    uint16_t h3 = (imm64 >> 48) & 0xFFFF;
    emit_movz_x(buf, rd, h0, 0);
    if (h1) emit_movk_x(buf, rd, h1, 1);
    if (h2) emit_movk_x(buf, rd, h2, 2);
    if (h3) emit_movk_x(buf, rd, h3, 3);
}

/* Same as emit_mov_imm64 but always emits 4 instructions, so that the
 * prologue has a fixed length (JIT_PROLOGUE_BYTES). */
static inline void emit_mov_imm64_fixed(uint32_t **buf, int rd, uint64_t imm64) {
    emit_movz_x(buf, rd, imm64 & 0xFFFF, 0);
    emit_movk_x(buf, rd, (imm64 >> 16) & 0xFFFF, 1);
    emit_movk_x(buf, rd, (imm64 >> 32) & 0xFFFF, 2);
    emit_movk_x(buf, rd, (imm64 >> 48) & 0xFFFF, 3);
}

/* 64-bit compare: SUBS xzr, rn, rm */
static inline void emit_cmp_x(uint32_t **buf, int rn, int rm) {
    emit32(buf, 0xEB00001F | (rm << 16) | (rn << 5));
}

/* ldrb wt, [rn, rm] */
static inline void emit_ldrb_reg(uint32_t **buf, int rt, int rn, int rm) {
    emit32(buf, 0x38606800 | (rm << 16) | (rn << 5) | rt);
}

static inline void emit_ldr_w(uint32_t **buf, int rt, int rn, uint32_t pimm) {
    emit32(buf, 0xB9400000 | (((pimm >> 2) & 0xFFF) << 10) | (rn << 5) | rt);
}

static inline void emit_str_w(uint32_t **buf, int rt, int rn, uint32_t pimm) {
    emit32(buf, 0xB9000000 | (((pimm >> 2) & 0xFFF) << 10) | (rn << 5) | rt);
}

static inline __attribute__((unused)) void emit_ldrb_w(uint32_t **buf, int rt, int rn, uint32_t pimm) {
    emit32(buf, 0x39400000 | ((pimm & 0xFFF) << 10) | (rn << 5) | rt);
}

static inline __attribute__((unused)) void emit_strb_w(uint32_t **buf, int rt, int rn, uint32_t pimm) {
    emit32(buf, 0x39000000 | ((pimm & 0xFFF) << 10) | (rn << 5) | rt);
}

static inline __attribute__((unused)) void emit_ldr_x(uint32_t **buf, int rt, int rn, uint32_t pimm) {
    emit32(buf, 0xF9400000 | (((pimm >> 3) & 0xFFF) << 10) | (rn << 5) | rt);
}

static inline __attribute__((unused)) void emit_str_x(uint32_t **buf, int rt, int rn, uint32_t pimm) {
    emit32(buf, 0xF9000000 | (((pimm >> 3) & 0xFFF) << 10) | (rn << 5) | rt);
}

static inline void emit_stp(uint32_t **buf, int rt1, int rt2, int rn, int32_t simm7) {
    emit32(buf, 0xA9000000 | (((simm7 >> 3) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline void emit_ldp(uint32_t **buf, int rt1, int rt2, int rn, int32_t simm7) {
    emit32(buf, 0xA9400000 | (((simm7 >> 3) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline void emit_stp_pre(uint32_t **buf, int rt1, int rt2, int rn, int32_t simm7) {
    emit32(buf, 0xA9800000 | (((simm7 >> 3) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline void emit_ldp_post(uint32_t **buf, int rt1, int rt2, int rn, int32_t simm7) {
    emit32(buf, 0xA8C00000 | (((simm7 >> 3) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

static inline __attribute__((unused)) void emit_b(uint32_t **buf, int32_t offset) {
    emit32(buf, 0x14000000 | ((offset >> 2) & 0x03FFFFFF));
}

static inline __attribute__((unused)) void emit_bl(uint32_t **buf, int32_t offset) {
    emit32(buf, 0x94000000 | ((offset >> 2) & 0x03FFFFFF));
}

static inline void emit_blr(uint32_t **buf, int rn) {
    emit32(buf, 0xD63F0000 | (rn << 5));
}

static inline void emit_ret(uint32_t **buf) {
    emit32(buf, 0xD65F03C0);
}

static inline void emit_b_cond(uint32_t **buf, int cond, int32_t offset) {
    emit32(buf, 0x54000000 | (((offset >> 2) & 0x7FFFF) << 5) | (cond & 0xF));
}

static inline void emit_cbz_w(uint32_t **buf, int rt, int32_t offset) {
    emit32(buf, 0x34000000 | (((offset >> 2) & 0x7FFFF) << 5) | rt);
}

static inline void emit_cbnz_w(uint32_t **buf, int rt, int32_t offset) {
    emit32(buf, 0x35000000 | (((offset >> 2) & 0x7FFFF) << 5) | rt);
}

static inline void emit_cset_w(uint32_t **buf, int rd, int cond) {
    emit32(buf, 0x1A9F07E0 | (((cond ^ 1) & 0xF) << 12) | rd);
}

/* --- Flag Calculation Helpers --- */

static void emit_flags_cf(uint32_t **buf, int bit_reg) {
    /* bit_reg bit 0 contains the new CF */
    emit_ldr_w(buf, REG_W13, REG_X19, CPU_OFF_FLAGS);
    emit_bfi_w(buf, REG_W13, bit_reg, 0, 1);
    emit_str_w(buf, REG_W13, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

static void emit_flags_zf(uint32_t **buf, int is_zero_reg) {
    /* is_zero_reg: 1 if zero, 0 if non-zero */
    emit_ldr_w(buf, REG_W13, REG_X19, CPU_OFF_FLAGS);
    emit_bfi_w(buf, REG_W13, is_zero_reg, 6, 1);
    emit_str_w(buf, REG_W13, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

static void emit_write_r8(uint32_t **buf, int rm, int val_reg) {
    int target_reg = (rm < 4) ? x86_to_arm[rm] : x86_to_arm[rm - 4];
    emit_bfi_w(buf, target_reg, val_reg, (rm < 4) ? 0 : 8, 8);
}

/*
 * EFLAGS Synchronizers:
 * Writes updated CF, ZF, SF, OF directly into cpu->flags (offset 40)
 * and clears cpu->cc.mask (offset 300) so the C interpreter reads cpu->flags directly.
 */
static void emit_flags_logic(uint32_t **buf, int res_reg) {
    /* CF = 0, OF = 0 */
    /* ZF = (res == 0) */
    emit_cmp_imm_w(buf, res_reg, 0);
    emit_cset_w(buf, REG_W9, COND_EQ);  /* ZF in bit 0 */
    emit_cset_w(buf, REG_W11, COND_MI); /* SF in bit 0 */
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W11, 1); /* SF in bit 1, ZF in bit 0 */
    emit_lsl_imm_w(buf, REG_W9, REG_W9, 6); /* SF at bit 7, ZF at bit 6 */

    /* Merge into cpu->flags: clear CF(0), ZF(6), SF(7), OF(11) = ~0x8C1 = 0xFFFFF73E */
    emit_mov_imm32(buf, REG_W14, 0xFFFFF73E);
    emit_ldr_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_and_w(buf, REG_W15, REG_W15, REG_W14);
    emit_orr_w(buf, REG_W15, REG_W15, REG_W9);
    emit_str_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

static void emit_flags_sub(uint32_t **buf, int res_reg, int r1_reg, int r2_reg) {
    (void)res_reg;
    /* Compare r1 and r2 to set AArch64 hardware condition codes */
    emit_subs_w(buf, REG_WZR, r1_reg, r2_reg);
    emit_cset_w(buf, REG_W9, COND_CC);  /* CF (borrow) at bit 0 */
    emit_cset_w(buf, REG_W11, COND_EQ); /* ZF at bit 0 */
    emit_cset_w(buf, REG_W12, COND_MI); /* SF at bit 0 */
    emit_cset_w(buf, REG_W13, COND_VS); /* OF at bit 0 */

    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W11, 6);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W12, 7);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W13, 11);

    emit_mov_imm32(buf, REG_W14, 0xFFFFF73E);
    emit_ldr_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_and_w(buf, REG_W15, REG_W15, REG_W14);
    emit_orr_w(buf, REG_W15, REG_W15, REG_W9);
    emit_str_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

static void emit_flags_add(uint32_t **buf, int res_reg, int r1_reg, int r2_reg) {
    (void)res_reg;
    /* Add r1 and r2 to set AArch64 hardware condition codes */
    emit_adds_w(buf, REG_WZR, r1_reg, r2_reg);
    emit_cset_w(buf, REG_W9, COND_CS);  /* CF (carry) at bit 0 */
    emit_cset_w(buf, REG_W11, COND_EQ); /* ZF at bit 0 */
    emit_cset_w(buf, REG_W12, COND_MI); /* SF at bit 0 */
    emit_cset_w(buf, REG_W13, COND_VS); /* OF at bit 0 */

    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W11, 6);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W12, 7);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W13, 11);

    emit_mov_imm32(buf, REG_W14, 0xFFFFF73E);
    emit_ldr_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_and_w(buf, REG_W15, REG_W15, REG_W14);
    emit_orr_w(buf, REG_W15, REG_W15, REG_W9);
    emit_str_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

static void emit_flags_inc(uint32_t **buf, int res_reg) {
    /* CF is untouched in INC! */
    emit_mov_imm32(buf, REG_W14, 0x80000000);
    emit_subs_w(buf, REG_WZR, res_reg, REG_W14);
    emit_cset_w(buf, REG_W12, COND_EQ); /* OF = (res == 0x80000000) */

    emit_cmp_imm_w(buf, res_reg, 0);
    emit_cset_w(buf, REG_W11, COND_EQ); /* ZF = (res == 0) */
    emit_cset_w(buf, REG_W13, COND_MI); /* SF = (res < 0) */

    emit_lsl_imm_w(buf, REG_W9, REG_W12, 11);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W11, 6);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W13, 7);

    /* Clear ZF(6), SF(7), OF(11) = ~0x8C0 = 0xFFFFF73F (CF is preserved) */
    emit_mov_imm32(buf, REG_W14, 0xFFFFF73F);
    emit_ldr_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_and_w(buf, REG_W15, REG_W15, REG_W14);
    emit_orr_w(buf, REG_W15, REG_W15, REG_W9);
    emit_str_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

static void emit_flags_dec(uint32_t **buf, int res_reg) {
    /* CF is untouched in DEC! */
    emit_mov_imm32(buf, REG_W14, 0x7FFFFFFF);
    emit_subs_w(buf, REG_WZR, res_reg, REG_W14);
    emit_cset_w(buf, REG_W12, COND_EQ); /* OF = (res == 0x7FFFFFFF) */

    emit_cmp_imm_w(buf, res_reg, 0);
    emit_cset_w(buf, REG_W11, COND_EQ); /* ZF = (res == 0) */
    emit_cset_w(buf, REG_W13, COND_MI); /* SF = (res < 0) */

    emit_lsl_imm_w(buf, REG_W9, REG_W12, 11);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W11, 6);
    emit_orr_shift_w(buf, REG_W9, REG_W9, REG_W13, 7);

    /* Clear ZF(6), SF(7), OF(11) = ~0x8C0 = 0xFFFFF73F (CF is preserved) */
    emit_mov_imm32(buf, REG_W14, 0xFFFFF73F);
    emit_ldr_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_and_w(buf, REG_W15, REG_W15, REG_W14);
    emit_orr_w(buf, REG_W15, REG_W15, REG_W9);
    emit_str_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
}

/* SHLD/SHRD (486; register form, mod==3 only). count_w holds the shift count
 * already clamped to 1..31 (the caller skips the instruction when count==0).
 * res = (x << count) | (y >> (32-count)) for SHLD, mirrored for SHRD, where
 * x = original destination value and y = source register.
 * CF = last bit shifted out of x, OF = MSB(x) ^ MSB(res), ZF/SF from res.
 * AF/PF are deliberately left stale, matching this backend's other flag paths.
 * The result is written to the destination last so a dst==src encoding still
 * leaves the shifted sum. Clobbers W1,W2,W9..W15; W16 is free scratch. */
static void emit_shxd(uint32_t **buf, int dst_w, int src_w, int count_w, int is_shld) {
    /* x = original dst -> W16 */
    emit_mov_w(buf, REG_W16, dst_w);
    /* 32 - count -> W2 */
    emit_mov_imm32(buf, REG_W2, 32);
    emit_sub_w(buf, REG_W2, REG_W2, count_w);
    /* res -> W9 */
    if (is_shld) {
        emit_lslv_w(buf, REG_W9, dst_w, count_w);
        emit_lsrv_w(buf, REG_W10, src_w, REG_W2);
    } else {
        emit_lsrv_w(buf, REG_W9, dst_w, count_w);
        emit_lslv_w(buf, REG_W10, src_w, REG_W2);
    }
    emit_orr_w(buf, REG_W9, REG_W9, REG_W10);
    /* CF -> W12 bit 0 */
    if (is_shld) {
        emit_lsrv_w(buf, REG_W10, REG_W16, REG_W2);
    } else {
        emit_sub_imm_w(buf, REG_W1, count_w, 1);
        emit_lsrv_w(buf, REG_W10, REG_W16, REG_W1);
    }
    emit_ubfx_w(buf, REG_W12, REG_W10, 0, 1);
    /* ZF (bit 6) / SF (bit 7) -> W11 */
    emit_cmp_imm_w(buf, REG_W9, 0);
    emit_cset_w(buf, REG_W10, COND_EQ);
    emit_cset_w(buf, REG_W13, COND_MI);
    emit_orr_shift_w(buf, REG_W11, REG_W10, REG_W13, 1);
    emit_lsl_imm_w(buf, REG_W11, REG_W11, 6);
    emit_orr_w(buf, REG_W12, REG_W12, REG_W11);
    /* OF = MSB(x) ^ MSB(res) at bit 11 */
    emit_eor_w(buf, REG_W13, REG_W16, REG_W9);
    emit_lsr_imm_w(buf, REG_W13, REG_W13, 31);
    emit_lsl_imm_w(buf, REG_W13, REG_W13, 11);
    emit_orr_w(buf, REG_W12, REG_W12, REG_W13);
    /* Merge: clear CF(0), ZF(6), SF(7), OF(11) = ~0x8C1 = 0xFFFFF73E */
    emit_mov_imm32(buf, REG_W14, 0xFFFFF73E);
    emit_ldr_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_and_w(buf, REG_W15, REG_W15, REG_W14);
    emit_orr_w(buf, REG_W15, REG_W15, REG_W12);
    emit_str_w(buf, REG_W15, REG_X19, CPU_OFF_FLAGS);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);
    /* write res last */
    emit_mov_w(buf, dst_w, REG_W9);
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

void jit_init(void) {
    if (s_initialized) return;

    {
        const char *env = getenv("TINY386_JIT_RING0");
        if (env) {
            s_jit_ring0_allowed = (env[0] != '0');
        } else {
            s_jit_ring0_allowed = 0;
        }
    }

    /* Allocate executable JIT buffer with MAP_JIT on macOS Apple Silicon */
    s_jit_buf = (uint8_t *)mmap(NULL, JIT_BUF_SIZE,
                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_ANON | MAP_PRIVATE | MAP_JIT,
                                -1, 0);
    if (s_jit_buf == MAP_FAILED) {
        perror("[JIT] mmap MAP_JIT failed");
        s_jit_buf = NULL;
        return;
    }

    printf("[JIT] AArch64 Code Buffer allocated at %p (%u MB, MAP_JIT)\n",
           s_jit_buf, (unsigned)(JIT_BUF_SIZE / (1024 * 1024)));

    /* Verify execution permissions and hardware instructions */
    if (s_jit_buf) {
        jit_write_enable();
        uint32_t *test_code = (uint32_t *)s_jit_buf;

        /* Probe 1: Basic addition and return: add w0, w0, #1; ret */
        test_code[0] = 0x11000400; /* add w0, w0, #1 */
        test_code[1] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 8);
        jit_write_disable();

        typedef int (*test_fn1)(int);
        int ret = ((test_fn1)s_jit_buf)(100);
        printf("[JIT] AArch64 execution self-test passed: 100 + 1 = %d\n", ret);

        /* Probe 2: Signed divide: sdiv w0, w0, w1; ret (100 / -7 = -14) */
        jit_write_enable();
        test_code[0] = 0x1AC10C00; /* sdiv w0, w0, w1 */
        test_code[1] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 8);
        jit_write_disable();

        typedef int (*test_fn2)(int, int);
        int div_ret = ((test_fn2)s_jit_buf)(100, -7);
        printf("[JIT] AArch64 hardware sdiv test: 100 / -7 = %d (expected -14) [SUPPORTED: %s]\n",
               div_ret, (div_ret == -14) ? "YES" : "NO");

        /* Probe 3: Hardware remainder: udiv w2, w0, w1; msub w0, w2, w1, w0; ret (100 % 7 = 2) */
        jit_write_enable();
        test_code[0] = 0x1AC10802; /* udiv w2, w0, w1 */
        test_code[1] = 0x1B018040; /* msub w0, w2, w1, w0 */
        test_code[2] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 12);
        jit_write_disable();

        int rem_ret = ((test_fn2)s_jit_buf)(100, 7);
        printf("[JIT] AArch64 hardware rem test: 100 %% 7 = %d (expected 2) [SUPPORTED: %s]\n",
               rem_ret, (rem_ret == 2) ? "YES" : "NO");

        /* Probe 4: Rotate right: ror w0, w0, #4 (0x80 -> 0x8) */
        jit_write_enable();
        test_code[0] = 0x13801000; /* ror w0, w0, #4 (extr w0, w0, w0, #4) */
        test_code[1] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 8);
        jit_write_disable();

        int ror_ret = ((test_fn1)s_jit_buf)(0x00000080);
        printf("[JIT] AArch64 ror test: ror(0x80, 4) = 0x%x (expected 0x8) [SUPPORTED: %s]\n",
               ror_ret, (ror_ret == 0x8) ? "YES" : "NO");

        /* Probe 5: Byte swap (BSWAP): rev w0, w0 (0x12345678 -> 0x78563412) */
        jit_write_enable();
        test_code[0] = 0x5AC00800; /* rev w0, w0 */
        test_code[1] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 8);
        jit_write_disable();

        uint32_t rev_ret = (uint32_t)((test_fn1)s_jit_buf)(0x12345678);
        printf("[JIT] AArch64 rev (bswap) test: rev(0x12345678) = 0x%08x (expected 0x78563412) [SUPPORTED: %s]\n",
               (unsigned)rev_ret, (rev_ret == 0x78563412) ? "YES" : "NO");

        /* Probe 6: SIB shifted add: add w0, w1, w0, lsl #2 (10 * 4 + 5 = 45) */
        jit_write_enable();
        test_code[0] = 0x0B000820; /* add w0, w1, w0, lsl #2 */
        test_code[1] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 8);
        jit_write_disable();

        int sib_ret = ((test_fn2)s_jit_buf)(10, 5);
        printf("[JIT] AArch64 SIB shifted add test: (10 * 4) + 5 = %d (expected 45) [SUPPORTED: %s]\n",
               sib_ret, (sib_ret == 45) ? "YES" : "NO");

        /* Probe 7: BFI bit field insert: bfi w0, w1, #8, #8 (0x12340078 with 0xAB -> 0x1234AB78) */
        jit_write_enable();
        test_code[0] = 0x33181C20; /* bfi w0, w1, #8, #8 */
        test_code[1] = 0xD65F03C0; /* ret */
        jit_sync_range(s_jit_buf, 8);
        jit_write_disable();

        uint32_t bfi_ret = (uint32_t)((test_fn2)s_jit_buf)(0x12340078, 0xAB);
        printf("[JIT] AArch64 BFI test: bfi(0x12340078, 0xAB, 8, 8) = 0x%08x (expected 0x1234ab78) [SUPPORTED: %s]\n",
               (unsigned)bfi_ret, (bfi_ret == 0x1234ab78) ? "YES" : "NO");

        jit_write_enable();
        memset(s_jit_buf, 0, 64);
        jit_sync_range(s_jit_buf, 64);
        jit_write_disable();
    }

    if (!s_hash_tab) s_hash_tab = (JITEntry (*)[JIT_HASH_WAYS])calloc(JIT_HASH_SETS * JIT_HASH_WAYS, sizeof(JITEntry));
    if (!s_hash_lru) s_hash_lru = (uint8_t *)calloc(JIT_HASH_SETS, 1);
    if (!s_hotspot_tag) s_hotspot_tag = (uint32_t *)calloc(JIT_HOTSPOT_SIZE, sizeof(uint32_t));
    if (!s_hotspot_cnt) s_hotspot_cnt = (uint8_t *)calloc(JIT_HOTSPOT_SIZE, 1);
    if (!jit_smc_page_map) jit_smc_page_map = calloc(JIT_SMC_MAP_PAGES, 1);
    if (!jit_page_gen) jit_page_gen = calloc(JIT_SMC_MAP_PAGES, 1);
    if (!s_entry_gen) s_entry_gen = (uint8_t *)calloc(JIT_HASH_SETS * JIT_HASH_WAYS, 1);
    if (!s_xlate) s_xlate = (JITXlate *)calloc(1u << JIT_XLATE_SHIFT, sizeof(JITXlate));

    /* Inline flat-TLB: 512 entries of {tag, pad, 64-bit host delta} = 8KB. */
    if (!jit_fast_tlb) {
        jit_fast_tlb = (JitFastTLBEntry *)malloc(
            sizeof(JitFastTLBEntry) * JIT_FAST_TLB_SIZE);
        if (jit_fast_tlb) {
            memset(jit_fast_tlb, 0xFF, sizeof(JitFastTLBEntry) * JIT_FAST_TLB_SIZE);
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
    if (s_xlate) memset(s_xlate, 0, (1u << JIT_XLATE_SHIFT) * sizeof(JITXlate));
    jit_smc_dirty = 0;
    jit_fast_tlb_flush(); /* blocks retired: no resident mapping may survive */
    s_xlate_epoch++; /* cached transitions must be revalidated after a flush */
    jit_flush_cache();
}

void jit_note_map_change(void) {
    s_xlate_epoch++;
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

    /* Fast path: direct-mapped transition cache. Skips the TLB probe + hash walk. */
    if (s_xlate) {
        JITXlate *x = &s_xlate[(guest_pc * 2654435761u) >> (32 - JIT_XLATE_SHIFT)];
        if (x->epoch == s_xlate_epoch && x->pc == guest_pc &&
            jit_page_gen_of(x->key) == x->gen) {
            s_jit_exec_count++;
            if (num_insns) *num_insns = x->insns;
            return s_jit_buf + x->code_off;
        }
    }

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
            uint32_t n = e->phys_and_insns & 0xFFFu;
            if (num_insns) *num_insns = (n > 0) ? n : 1;
            if (s_xlate) {
                JITXlate *x = &s_xlate[(guest_pc * 2654435761u) >> (32 - JIT_XLATE_SHIFT)];
                x->pc = guest_pc;
                x->code_off = e->code_off;
                x->key = key;
                x->epoch = s_xlate_epoch;
                x->insns = (n > 0) ? n : 1;
                x->gen = gen;
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
            /* A stale entry (page generation no longer matches) must not block
             * recompilation; only a fresh block makes this PC "already done". */
            if (e->phys_and_insns != 0 && e->guest_pc == guest_pc &&
                (e->phys_and_insns >> 12) == key &&
                s_entry_gen && s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
                return false;
            }
        }
    }
    uint32_t idx = (guest_pc * 2654435761u) >> (32 - JIT_HOTSPOT_SHIFT);
    if (s_hotspot_tag[idx] != guest_pc) {
        /* Different PC aliased this slot: start a fresh exact count. */
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
/*
 * Frame Layout (112 bytes total, 16-byte aligned):
 * [sp, #0]:  x29, x30 (FP, LR)
 * [sp, #16]: x19, x20 (Host CPU pointer, EAX)
 * [sp, #32]: x21, x22 (ECX, EDX)
 * [sp, #48]: x23, x24 (EBX, ESP)
 * [sp, #64]: x25, x26 (EBP, ESI)
 * [sp, #80]: x27, x28 (EDI, x28)
 * [sp, #96]: temporary uint32_t buffer for memory loads (e.g. cpu_load32)
 */
static void emit_prologue(uint32_t **buf) {
    emit_stp_pre(buf, REG_X29, REG_X30, REG_SP, -JIT_FRAME_SIZE);
    emit_mov_x(buf, REG_X29, REG_SP);
    emit_stp(buf, REG_X19, REG_X20, REG_SP, 16);
    emit_stp(buf, REG_X21, REG_X22, REG_SP, 32);
    emit_stp(buf, REG_X23, REG_X24, REG_SP, 48);
    emit_stp(buf, REG_X25, REG_X26, REG_SP, 64);
    emit_stp(buf, REG_X27, REG_X28, REG_SP, 80);

    /* x19 = x0 (CPUI386 *cpu) */
    emit_mov_x(buf, REG_X19, REG_X0);

    /* x28 = absolute cpu->cycle deadline for this chain session. A block tail
     * returns to the dispatcher once cpu->cycle reaches it, which is how the
     * dispatcher recovers the consumed step count without C-side accounting. */
    emit_mov_imm64_fixed(buf, REG_X9, (uint64_t)(uintptr_t)&jit_cycle_deadline);
    emit_ldr_x(buf, REG_X28, REG_X9, 0);

    /* Cache the two addresses chain slots need to validate a link, so each
     * slot costs a single load instead of a 4-instruction address materialise. */
    emit_mov_imm64_fixed(buf, REG_X9, (uint64_t)(uintptr_t)&s_xlate_epoch);
    emit_str_x(buf, REG_X9, REG_SP, JIT_SLOT_GUARD);
    emit_mov_imm64_fixed(buf, REG_X9, (uint64_t)(uintptr_t)&jit_page_gen);
    emit_ldr_x(buf, REG_X9, REG_X9, 0);          /* x9 = jit_page_gen array */
    emit_str_x(buf, REG_X9, REG_SP, JIT_SLOT_PAGEGEN);

    /* Cache the inline flat-TLB base pointer (loaded indirectly so a block is
     * correct even if the table is (re)allocated later). */
    emit_mov_imm64_fixed(buf, REG_X9, (uint64_t)(uintptr_t)&jit_fast_tlb);
    emit_ldr_x(buf, REG_X9, REG_X9, 0);
    emit_str_x(buf, REG_X9, REG_SP, JIT_SLOT_TLB);

    /* Load x86 GPRs from cpu->gprx[0..7] */
    for (int i = 0; i < 8; i++) {
        emit_ldr_w(buf, x86_to_arm[i], REG_X19, CPU_OFF_GPR(i));
    }
}

/* Add `n` retired guest instructions to cpu->cycle. cpu->cycle is a signed
 * 64-bit 'long' on every JIT-capable target, hence the x-register form. */
static void emit_cycle_add(uint32_t **buf, uint32_t n) {
    emit_ldr_x(buf, REG_X9, REG_X19, CPU_OFF_CYCLE);
    if (n) emit_add_imm_x(buf, REG_X9, REG_X9, n);
    emit_str_x(buf, REG_X9, REG_X19, CPU_OFF_CYCLE);
}

/* Patch a placeholder b.cond / cbz / cbnz at `insn` with a byte offset. */
static inline void jit_patch_bcond(uint32_t *insn, int32_t off) {
    *insn = (*insn & ~0x00FFFFE0u) | ((((uint32_t)(off >> 2)) & 0x7FFFFu) << 5);
}

/* Patch a placeholder b at `insn` with a byte offset. */
static inline void jit_patch_b(uint32_t *insn, int32_t off) {
    *insn = (*insn & 0xFC000000u) | ((uint32_t)(off >> 2) & 0x03FFFFFFu);
}

/* Write back the x86 GPRs, publish the exit EIP, tear down the frame and return
 * to the C dispatcher. */
static void emit_exit_body(uint32_t **buf, uint32_t exit_eip) {
    for (int i = 0; i < 8; i++) {
        emit_str_w(buf, x86_to_arm[i], REG_X19, CPU_OFF_GPR(i));
    }

    emit_mov_imm32(buf, REG_W9, exit_eip);
    emit_str_w(buf, REG_W9, REG_X19, CPU_OFF_IP);
    emit_str_w(buf, REG_W9, REG_X19, CPU_OFF_NEXT_IP);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);

    emit_ldp(buf, REG_X19, REG_X20, REG_SP, 16);
    emit_ldp(buf, REG_X21, REG_X22, REG_SP, 32);
    emit_ldp(buf, REG_X23, REG_X24, REG_SP, 48);
    emit_ldp(buf, REG_X25, REG_X26, REG_SP, 64);
    emit_ldp(buf, REG_X27, REG_X28, REG_SP, 80);
    emit_ldp_post(buf, REG_X29, REG_X30, REG_SP, JIT_FRAME_SIZE);

    emit_ret(buf);
}

static void emit_exit_body_reg(uint32_t **buf, int arm_reg_eip) {
    for (int i = 0; i < 8; i++) {
        emit_str_w(buf, x86_to_arm[i], REG_X19, CPU_OFF_GPR(i));
    }

    emit_str_w(buf, arm_reg_eip, REG_X19, CPU_OFF_IP);
    emit_str_w(buf, arm_reg_eip, REG_X19, CPU_OFF_NEXT_IP);
    emit_str_w(buf, REG_WZR, REG_X19, CPU_OFF_CC_MASK);

    emit_ldp(buf, REG_X19, REG_X20, REG_SP, 16);
    emit_ldp(buf, REG_X21, REG_X22, REG_SP, 32);
    emit_ldp(buf, REG_X23, REG_X24, REG_SP, 48);
    emit_ldp(buf, REG_X25, REG_X26, REG_SP, 64);
    emit_ldp(buf, REG_X27, REG_X28, REG_SP, 80);
    emit_ldp_post(buf, REG_X29, REG_X30, REG_SP, JIT_FRAME_SIZE);

    emit_ret(buf);
}

/* Mid-block fault exit: the guest instruction count is not final yet, so no
 * cycle accounting is emitted here. The dispatcher detects a zero cycle delta
 * and falls back to the block's full instruction count. */
static void emit_epilogue(uint32_t **buf, uint32_t exit_eip) {
    emit_exit_body(buf, exit_eip);
}

/* Tail exit with a dynamic (register) target, e.g. RET. */
static void emit_epilogue_reg(uint32_t **buf, int arm_reg_eip, uint32_t n) {
    emit_cycle_add(buf, n);
    emit_exit_body_reg(buf, arm_reg_eip);
}

/*
 * Chainable block tail. `succ_body_off` is the buffer offset of the successor
 * block's body (past its prologue), or JIT_NO_CHAIN when the successor has no
 * compiled (and valid) block yet. The successor's physical page identity
 * (`succ_key`) and the page generation observed when the link was created
 * (`succ_gen`) guard against guest self-modifying code; `link_epoch` is the
 * mapping epoch at link time and guards against a VA->PA change (only when the
 * successor is a different block, since a self-loop's own mapping was already
 * validated by the lookup that entered it).
 *
 * The link is deliberately not patched or unlinked afterwards: if any guard
 * fails the tail simply falls through into the normal exit body, and the
 * dispatcher re-validates and re-dispatches the successor. jit_flush() is safe
 * because it retires every block, so no stale slot can be reached.
 */
static void emit_tail(uint32_t **buf, uint32_t exit_eip, uint32_t n,
                      uint32_t succ_body_off, uint32_t succ_key, uint8_t succ_gen,
                      uint32_t link_epoch, bool self_link)
{
    emit_cycle_add(buf, n);

    uint32_t *bails[4];
    int nbail = 0;

    /* Stop once this chain session has consumed the whole step budget. */
    emit_cmp_x(buf, REG_X9, REG_X28);
    bails[nbail] = *buf; emit_b_cond(buf, COND_GE, 0); nbail++;

    /* Stop when an interrupt is pending so pc_step can service it. */
    emit_ldr_w(buf, REG_W10, REG_X19, CPU_OFF_INTR);
    bails[nbail] = *buf; emit_cbnz_w(buf, REG_W10, 0); nbail++;

    uint32_t *chain_jump = NULL;
    if (succ_body_off != JIT_NO_CHAIN) {
        if (!self_link) {
            /* Mapping epoch unchanged since the link was created. */
            emit_ldr_x(buf, REG_X11, REG_SP, JIT_SLOT_GUARD);
            emit_ldr_w(buf, REG_W10, REG_X11, 0);
            emit_mov_imm32(buf, REG_W12, link_epoch);
            emit_cmp_w(buf, REG_W10, REG_W12);
            bails[nbail] = *buf; emit_b_cond(buf, COND_NE, 0); nbail++;
        }
        /* Successor's page generation unchanged (no self-modifying code). */
        emit_ldr_x(buf, REG_X11, REG_SP, JIT_SLOT_PAGEGEN);
        emit_mov_imm32(buf, REG_W12, succ_key);
        emit_ldrb_reg(buf, REG_W10, REG_X11, REG_W12);
        emit_cmp_imm_w(buf, REG_W10, succ_gen);
        bails[nbail] = *buf; emit_b_cond(buf, COND_NE, 0); nbail++;

        chain_jump = *buf; emit_b(buf, 0);
    }

    uint32_t *exit_body = *buf;
    for (int i = 0; i < nbail; i++) {
        jit_patch_bcond(bails[i], (int32_t)((uint8_t *)exit_body - (uint8_t *)bails[i]));
    }
    if (chain_jump) {
        jit_patch_b(chain_jump, (int32_t)((uint8_t *)(s_jit_buf + succ_body_off) -
                                           (uint8_t *)chain_jump));
    }

    emit_exit_body(buf, exit_eip);
}

/*
 * Checks W0 after cpu_load32/store32:
 * If W0 != 0 (success): branches over the fault epilogue to next instruction.
 * If W0 == 0 (fault): falls through into fault epilogue (exiting with faulting EIP).
 */
static void emit_mem_fault_check(uint32_t **buf, uint32_t insn_eip) {
    uint32_t *branch_success = *buf;
    emit_cbnz_w(buf, REG_W0, 0); /* placeholder */
    emit_epilogue(buf, insn_eip);
    uint32_t *success_target = *buf;
    int32_t offset = (int32_t)((uint8_t *)success_target - (uint8_t *)branch_success);
    uint32_t *p = branch_success;
    emit_cbnz_w(&p, REG_W0, offset);
}

/* Inline flat-TLB 32-bit load. The effective address must already be in REG_W2.
 * On a table hit this is a single native ldr w straight into dst_reg; every
 * other case (non-flat segment, misaligned offset, table miss, no fast-TLB)
 * branches to a shared cpu_load32() slow path that also refills the table, then
 * copies the result into dst_reg.  dst_reg must avoid W10-W13/X9/X10/X13/X16,
 * which the probe uses as scratch (W16 is reused by the slow path). */
static void emit_mem_load32(uint32_t **buf, int dst_reg, int seg, uint32_t insn_eip) {
    if (jit_fast_tlb) {
        uint32_t *bpatch[4]; int nb = 0;
        uint32_t *fast_jump;
        if (seg != 1 /*SEG_CS*/ && seg != 2 /*SEG_SS*/) {
            emit_ldr_w(buf, REG_W10, REG_X19, CPU_OFF_SEG_BASE(seg));
            bpatch[nb++] = *buf;
            emit_cbnz_w(buf, REG_W10, 0);
        }
        emit_ubfx_w(buf, REG_W10, REG_W2, 0, 2);              /* misaligned? */
        bpatch[nb++] = *buf;
        emit_cbnz_w(buf, REG_W10, 0);

        emit_ldr_x(buf, REG_X9, REG_SP, JIT_SLOT_TLB);
        emit_ubfx_w(buf, REG_W10, REG_W2, 12, 9);             /* index */
        emit_ubfx_w(buf, REG_W11, REG_W2, 12, 20);            /* vpage */
        emit_lsl_imm_w(buf, REG_W10, REG_W10, 4);             /* 16-byte entries */
        emit_add_x(buf, REG_X10, REG_X9, REG_X10);            /* &entry */
        emit_ldr_w(buf, REG_W12, REG_X10, 0);                 /* resident tag */
        emit_cmp_w(buf, REG_W11, REG_W12);
        bpatch[nb++] = *buf;
        emit_b_cond(buf, COND_NE, 0);
        emit_ldr_x(buf, REG_X13, REG_X10, 8);                 /* host_delta */
        emit_add_x(buf, REG_X13, REG_X13, REG_X2);            /* host ptr */
        emit_ldr_w(buf, dst_reg, REG_X13, 0);
        fast_jump = *buf;
        emit_b(buf, 0);                                       /* -> done */

        /* ---- slow path: all misses converge here ---- */
        uint32_t *slow = *buf;
        emit_mov_x(buf, REG_X0, REG_X19);
        emit_mov_imm32(buf, REG_W1, seg);
        emit_add_imm_x(buf, REG_X3, REG_SP, JIT_SLOT_LOAD);
        emit_mov_imm64(buf, REG_X16, (uint64_t)cpu_load32);
        emit_blr(buf, REG_X16);
        emit_mem_fault_check(buf, insn_eip);
        emit_ldr_w(buf, dst_reg, REG_SP, JIT_SLOT_LOAD);

        uint32_t *done = *buf;
        for (int i = 0; i < nb; i++) {
            jit_patch_bcond(bpatch[i],
                            (int32_t)((uint8_t *)slow - (uint8_t *)bpatch[i]));
        }
        jit_patch_b(fast_jump, (int32_t)((uint8_t *)done - (uint8_t *)fast_jump));
    } else {
        /* Load: cpu_load32(cpu, seg, addr, SP + JIT_SLOT_LOAD) */
        emit_mov_x(buf, REG_X0, REG_X19);
        emit_mov_imm32(buf, REG_W1, seg);
        emit_add_imm_x(buf, REG_X3, REG_SP, JIT_SLOT_LOAD);
        emit_mov_imm64(buf, REG_X16, (uint64_t)cpu_load32);
        emit_blr(buf, REG_X16);
        emit_mem_fault_check(buf, insn_eip);
        emit_ldr_w(buf, dst_reg, REG_SP, JIT_SLOT_LOAD);
    }
}

/* 32-bit store. The effective address must already be in REG_W2 and the data
 * in src_reg (the copy into W3 happens before X16 is reloaded, so W16 is a
 * legal src_reg).
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
    emit_mov_x(buf, REG_X0, REG_X19);
    emit_mov_imm32(buf, REG_W1, seg);
    emit_mov_w(buf, REG_W3, src_reg);
    emit_mov_imm64(buf, REG_X16, (uint64_t)cpu_store32);
    emit_blr(buf, REG_X16);
    emit_mem_fault_check(buf, insn_eip);
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
 * @brief Dynamic Basic Block Compiler for AArch64
 */
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
        *body_off_out = blk_start_off + JIT_PROLOGUE_BYTES;
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
            s_entry_gen[set * JIT_HASH_WAYS + way] == gen) {
            *key_out = key;
            *gen_out = gen;
            *body_off_out = e->code_off + JIT_PROLOGUE_BYTES;
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

void *jit_compile_block(CPUI386 *cpu, uint32_t start_eip) {
    if (!s_initialized) jit_init();

    /* Reserve 1.5KB per block */
    if (s_jit_off + 1536 >= JIT_BUF_SIZE) {
        jit_flush();
    }

    uint32_t *code_start = (uint32_t *)(s_jit_buf + s_jit_off);
    uint32_t *code_ptr = code_start;
    uint32_t blk_start_off = (uint32_t)((uint8_t *)code_start - s_jit_buf);

    jit_write_enable();

    emit_prologue(&code_ptr);

    uint32_t cur_eip = start_eip;
    int insn_count = 0;
    int max_insns = 32;

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
            emit_mov_x(&code_ptr, REG_X0, REG_X19);
            emit_mov_imm32(&code_ptr, REG_W1, 2 /* SEG_SS */);
            emit_sub_imm_w(&code_ptr, REG_W2, REG_W24 /* ESP */, 4);
            emit_mov_w(&code_ptr, REG_W3, x86_to_arm[rd]);
            emit_mov_imm64(&code_ptr, REG_X16, (uint64_t)cpu_store32);
            emit_blr(&code_ptr, REG_X16);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_sub_imm_w(&code_ptr, REG_W24, REG_W24, 4);
            cur_eip += 1;
            insn_count++;
            continue;
        }

        /* POP r32 (0x58 + reg) */
        if (op >= 0x58 && op <= 0x5F) {
            int rd = op - 0x58;
            uint32_t insn_eip = cur_eip;
            emit_mov_x(&code_ptr, REG_X0, REG_X19);
            emit_mov_imm32(&code_ptr, REG_W1, 2 /* SEG_SS */);
            emit_mov_w(&code_ptr, REG_W2, REG_W24 /* ESP */);
            emit_add_imm_x(&code_ptr, REG_X3, REG_SP, 96);
            emit_mov_imm64(&code_ptr, REG_X16, (uint64_t)cpu_load32);
            emit_blr(&code_ptr, REG_X16);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_ldr_w(&code_ptr, x86_to_arm[rd], REG_SP, 96);
            emit_add_imm_w(&code_ptr, REG_W24, REG_W24, 4);
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

            emit_mov_x(&code_ptr, REG_X0, REG_X19);
            emit_mov_imm32(&code_ptr, REG_W1, 2 /* SEG_SS */);
            emit_sub_imm_w(&code_ptr, REG_W2, REG_W24 /* ESP */, 4);
            emit_mov_imm32(&code_ptr, REG_W3, return_eip);
            emit_mov_imm64(&code_ptr, REG_X16, (uint64_t)cpu_store32);
            emit_blr(&code_ptr, REG_X16);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_sub_imm_w(&code_ptr, REG_W24, REG_W24, 4);
            cur_eip = target_eip;
            insn_count++;
            goto block_done;
        }

        /* RET (0xC3) */
        if (op == 0xC3) {
            uint32_t insn_eip = cur_eip;
            emit_mov_x(&code_ptr, REG_X0, REG_X19);
            emit_mov_imm32(&code_ptr, REG_W1, 2 /* SEG_SS */);
            emit_mov_w(&code_ptr, REG_W2, REG_W24 /* ESP */);
            emit_add_imm_x(&code_ptr, REG_X3, REG_SP, 96);
            emit_mov_imm64(&code_ptr, REG_X16, (uint64_t)cpu_load32);
            emit_blr(&code_ptr, REG_X16);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_ldr_w(&code_ptr, REG_W10, REG_SP, 96);
            emit_add_imm_w(&code_ptr, REG_W24, REG_W24, 4);
            insn_count++;
            emit_epilogue_reg(&code_ptr, REG_W10, (uint32_t)insn_count);
            goto block_finished;
        }

        /* RET imm16 (0xC2) */
        if (op == 0xC2) {
            uint16_t imm16 = jit_read_u8(cpu, cur_eip + 1) | (jit_read_u8(cpu, cur_eip + 2) << 8);
            uint32_t insn_eip = cur_eip;
            emit_mov_x(&code_ptr, REG_X0, REG_X19);
            emit_mov_imm32(&code_ptr, REG_W1, 2 /* SEG_SS */);
            emit_mov_w(&code_ptr, REG_W2, REG_W24 /* ESP */);
            emit_add_imm_x(&code_ptr, REG_X3, REG_SP, 96);
            emit_mov_imm64(&code_ptr, REG_X16, (uint64_t)cpu_load32);
            emit_blr(&code_ptr, REG_X16);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_ldr_w(&code_ptr, REG_W10, REG_SP, 96);
            emit_add_imm_w(&code_ptr, REG_W24, REG_W24, 4 + imm16);
            insn_count++;
            emit_epilogue_reg(&code_ptr, REG_W10, (uint32_t)insn_count);
            goto block_finished;
        }

        /* MOV r32, imm32 (0xB8 + reg) */
        if (op >= 0xB8 && op <= 0xBF) {
            int rd = op - 0xB8;
            uint32_t imm = jit_read_u32(cpu, cur_eip + 1);
            emit_mov_imm32(&code_ptr, x86_to_arm[rd], imm);
            cur_eip += 5;
            insn_count++;
            continue;
        }

        /* INC r32 (0x40 + reg) */
        if (op >= 0x40 && op <= 0x47) {
            int rd = op - 0x40;
            emit_add_imm_w(&code_ptr, x86_to_arm[rd], x86_to_arm[rd], 1);
            emit_flags_inc(&code_ptr, x86_to_arm[rd]);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* DEC r32 (0x48 + reg) */
        if (op >= 0x48 && op <= 0x4F) {
            int rd = op - 0x48;
            emit_sub_imm_w(&code_ptr, x86_to_arm[rd], x86_to_arm[rd], 1);
            emit_flags_dec(&code_ptr, x86_to_arm[rd]);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* CWDE (0x98) - Sign-extend AX into EAX */
        if (op == 0x98) {
            emit_sxth_w(&code_ptr, REG_W20, REG_W20);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* CDQ (0x99) - Sign-extend EAX into EDX:EAX */
        if (op == 0x99) {
            emit_asr_imm_w(&code_ptr, REG_W22, REG_W20, 31);
            cur_eip += 1;
            insn_count++;
            last_cmp_type = CMP_NONE;
            continue;
        }

        /* XCHG r32, EAX (0x91 .. 0x97) */
        if (op >= 0x91 && op <= 0x97) {
            int rd = op - 0x90;
            emit_mov_w(&code_ptr, REG_W10, REG_W20);
            emit_mov_w(&code_ptr, REG_W20, x86_to_arm[rd]);
            emit_mov_w(&code_ptr, x86_to_arm[rd], REG_W10);
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
                emit_mov_w(&code_ptr, REG_W10, x86_to_arm[rm]);
                emit_mov_w(&code_ptr, x86_to_arm[rm], x86_to_arm[reg]);
                emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* MOV r/m32, r32 (0x89) */
            if (op == 0x89) {
                emit_mov_w(&code_ptr, x86_to_arm[rm], x86_to_arm[reg]);
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* MOV r32, r/m32 (0x8B) */
            if (op == 0x8B) {
                emit_mov_w(&code_ptr, x86_to_arm[reg], x86_to_arm[rm]);
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* ADD r/m32, r32 (0x01) */
            if (op == 0x01) {
                emit_add_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_add(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* ADD r32, r/m32 (0x03) */
            if (op == 0x03) {
                emit_add_w(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_flags_add(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* SUB r/m32, r32 (0x29) */
            if (op == 0x29) {
                emit_sub_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* SUB r32, r/m32 (0x2B) */
            if (op == 0x2B) {
                emit_sub_w(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* AND r/m32, r32 (0x21) */
            if (op == 0x21) {
                emit_and_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_logic(&code_ptr, REG_W10);
                emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* AND r32, r/m32 (0x23) */
            if (op == 0x23) {
                emit_and_w(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_flags_logic(&code_ptr, REG_W10);
                emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* OR r/m32, r32 (0x09) */
            if (op == 0x09) {
                emit_orr_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_logic(&code_ptr, REG_W10);
                emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* OR r32, r/m32 (0x0B) */
            if (op == 0x0B) {
                emit_orr_w(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_flags_logic(&code_ptr, REG_W10);
                emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* XOR r/m32, r32 (0x31) */
            if (op == 0x31) {
                emit_eor_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_logic(&code_ptr, REG_W10);
                emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* XOR r32, r/m32 (0x33) */
            if (op == 0x33) {
                emit_eor_w(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_flags_logic(&code_ptr, REG_W10);
                emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            /* CMP r/m32, r32 (0x39) */
            if (op == 0x39) {
                emit_sub_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                last_cmp_type = CMP_REG_REG;
                cmp_r1 = x86_to_arm[rm];
                cmp_r2 = x86_to_arm[reg];
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* CMP r32, r/m32 (0x3B) */
            if (op == 0x3B) {
                emit_sub_w(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[reg], x86_to_arm[rm]);
                last_cmp_type = CMP_REG_REG;
                cmp_r1 = x86_to_arm[reg];
                cmp_r2 = x86_to_arm[rm];
                cur_eip += 2;
                insn_count++;
                continue;
            }
            /* TEST r/m32, r32 (0x85) */
            if (op == 0x85) {
                emit_and_w(&code_ptr, REG_W10, x86_to_arm[rm], x86_to_arm[reg]);
                emit_flags_logic(&code_ptr, REG_W10);
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
                    emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                    emit_add_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_add(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 1: /* OR */
                    emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                    emit_orr_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_logic(&code_ptr, REG_W10);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 4: /* AND */
                    emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                    emit_and_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_logic(&code_ptr, REG_W10);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 5: /* SUB */
                    emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                    emit_sub_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 6: /* XOR */
                    emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                    emit_eor_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_logic(&code_ptr, REG_W10);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 7: /* CMP */
                    emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                    emit_sub_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    last_cmp_type = CMP_REG_IMM;
                    cmp_r1 = x86_to_arm[rm];
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
                    emit_mov_imm32(&code_ptr, REG_W14, imm32);
                    emit_add_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_add(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 1: /* OR */
                    emit_mov_imm32(&code_ptr, REG_W14, imm32);
                    emit_orr_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_logic(&code_ptr, REG_W10);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 4: /* AND */
                    emit_mov_imm32(&code_ptr, REG_W14, imm32);
                    emit_and_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_logic(&code_ptr, REG_W10);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 5: /* SUB */
                    emit_mov_imm32(&code_ptr, REG_W14, imm32);
                    emit_sub_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 6: /* XOR */
                    emit_mov_imm32(&code_ptr, REG_W14, imm32);
                    emit_eor_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_logic(&code_ptr, REG_W10);
                    emit_mov_w(&code_ptr, x86_to_arm[rm], REG_W10);
                    last_cmp_type = CMP_NONE;
                    break;
                case 7: /* CMP */
                    emit_mov_imm32(&code_ptr, REG_W14, imm32);
                    emit_sub_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                    last_cmp_type = CMP_REG_IMM;
                    cmp_r1 = x86_to_arm[rm];
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
                emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)(int32_t)simm8);
                emit_mul_w(&code_ptr, x86_to_arm[reg], x86_to_arm[rm], REG_W14);
                cur_eip += 3;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }

            /* IMUL r32, r/m32, imm32 (0x69) */
            if (op == 0x69) {
                uint32_t imm32 = jit_read_u32(cpu, cur_eip + 2);
                emit_mov_imm32(&code_ptr, REG_W14, imm32);
                emit_mul_w(&code_ptr, x86_to_arm[reg], x86_to_arm[rm], REG_W14);
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
                        emit_mov_imm32(&code_ptr, REG_W14, imm32);
                        emit_and_w(&code_ptr, REG_W10, x86_to_arm[rm], REG_W14);
                        emit_flags_logic(&code_ptr, REG_W10);
                        last_cmp_type = CMP_TEST;
                        cur_eip += 6;
                        insn_count++;
                        continue;
                    }
                case 2: /* NOT r/m32 */
                    emit_mvn_w(&code_ptr, x86_to_arm[rm], x86_to_arm[rm]);
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
            int base_arm = -1;
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
                        base_arm = -1;
                    } else {
                        /* [base + index*scale] */
                        disp = 0;
                        insn_len = 3;
                        base_arm = x86_to_arm[sib_base];
                    }
                    valid_mem = true;
                } else if (mod == 1) {
                    disp = (int8_t)jit_read_u8(cpu, cur_eip + 3);
                    insn_len = 4;
                    base_arm = x86_to_arm[sib_base];
                    valid_mem = true;
                } else if (mod == 2) {
                    disp = (int32_t)jit_read_u32(cpu, cur_eip + 3);
                    insn_len = 7;
                    base_arm = x86_to_arm[sib_base];
                    valid_mem = true;
                }
                /* Default segment for SIB: ESP(4) or EBP(5) uses SS, others DS */
                if (base_arm != -1 && (sib_base == 4 || sib_base == 5)) {
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
                        base_arm = -1;
                    } else {
                        /* [base] */
                        disp = 0;
                        insn_len = 2;
                        base_arm = x86_to_arm[rm];
                    }
                    valid_mem = true;
                } else if (mod == 1) {
                    disp = (int8_t)jit_read_u8(cpu, cur_eip + 2);
                    insn_len = 3;
                    base_arm = x86_to_arm[rm];
                    valid_mem = true;
                } else if (mod == 2) {
                    disp = (int32_t)jit_read_u32(cpu, cur_eip + 2);
                    insn_len = 6;
                    base_arm = x86_to_arm[rm];
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

                /* Compute effective address into REG_W2 */
                if (has_sib && sib_index != 4) {
                    int index_arm = x86_to_arm[sib_index];
                    if (base_arm != -1) {
                        /* AArch64 shifted register ADD: base + (index << sib_scale) */
                        emit_add_shift_w(&code_ptr, REG_W2, base_arm, index_arm, sib_scale);
                    } else {
                        if (sib_scale == 0) {
                            emit_mov_w(&code_ptr, REG_W2, index_arm);
                        } else {
                            emit_lsl_imm_w(&code_ptr, REG_W2, index_arm, sib_scale);
                        }
                    }
                    if (disp != 0) {
                        if (disp > 0 && disp <= 4095) {
                            emit_add_imm_w(&code_ptr, REG_W2, REG_W2, disp);
                        } else if (disp < 0 && -disp <= 4095) {
                            emit_sub_imm_w(&code_ptr, REG_W2, REG_W2, -disp);
                        } else {
                            emit_mov_imm32(&code_ptr, REG_W9, (uint32_t)disp);
                            emit_add_w(&code_ptr, REG_W2, REG_W2, REG_W9);
                        }
                    }
                } else {
                    if (base_arm == -1) {
                        emit_mov_imm32(&code_ptr, REG_W2, (uint32_t)disp);
                    } else if (disp == 0) {
                        emit_mov_w(&code_ptr, REG_W2, base_arm);
                    } else if (disp > 0 && disp <= 4095) {
                        emit_add_imm_w(&code_ptr, REG_W2, base_arm, disp);
                    } else if (disp < 0 && -disp <= 4095) {
                        emit_sub_imm_w(&code_ptr, REG_W2, base_arm, -disp);
                    } else {
                        emit_mov_imm32(&code_ptr, REG_W9, (uint32_t)disp);
                        emit_add_w(&code_ptr, REG_W2, base_arm, REG_W9);
                    }
                }

                if (op == 0x8D) {
                    /* LEA: destination register gets effective address directly */
                    emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W2);
                    cur_eip += insn_len;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                } else if (op == 0x8B) {
                    /* MOV r32, r/m32 */
                    emit_mem_load32(&code_ptr, x86_to_arm[reg], seg, insn_eip);
                } else if (op == 0x89) {
                    /* MOV r/m32, r32 */
                    emit_mem_store32(&code_ptr, x86_to_arm[reg], seg, insn_eip);
                } else if (op == 0x01 || op == 0x09 || op == 0x21 ||
                           op == 0x29 || op == 0x31) {
                    /* Read-modify-write r/m32, r32.  The EA in REG_W2 is spilled
                     * to JIT_SLOT_EA because the load slow path is a C call that
                     * clobbers the caller-saved registers (including x2).  The
                     * result is parked in REG_W16, which is a safe store source
                     * (the store helper only scratches W10-W13/X9/X10/X13 and
                     * reads W16 before reusing X16 on its slow path). */
                    emit_str_w(&code_ptr, REG_W2, REG_SP, JIT_SLOT_EA);
                    emit_mem_load32(&code_ptr, REG_W16, seg, insn_eip);
                    switch (op) {
                    case 0x01: /* ADD r/m32, r32 */
                        emit_add_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_add(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        break;
                    case 0x09: /* OR r/m32, r32 */
                        emit_orr_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_logic(&code_ptr, REG_W10);
                        break;
                    case 0x21: /* AND r/m32, r32 */
                        emit_and_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_logic(&code_ptr, REG_W10);
                        break;
                    case 0x29: /* SUB r/m32, r32 */
                        emit_sub_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_sub(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        break;
                    case 0x31: /* XOR r/m32, r32 */
                        emit_eor_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_logic(&code_ptr, REG_W10);
                        break;
                    }
                    emit_mov_w(&code_ptr, REG_W16, REG_W10);
                    emit_ldr_w(&code_ptr, REG_W2, REG_SP, JIT_SLOT_EA);
                    emit_mem_store32(&code_ptr, REG_W16, seg, insn_eip);
                    last_cmp_type = CMP_NONE;
                } else if (op == 0x81 || op == 0x83) {
                    /* Group-1 immediate with a memory destination.  /7 (CMP) is
                     * read-only; /0,/1,/4,/5,/6 are read-modify-write.  /2 (ADC)
                     * and /3 (SBB) need a carry-in the backend does not model, so
                     * they fall back like their register forms do. */
                    int subop = reg;
                    if (subop == 7) {
                        emit_mem_load32(&code_ptr, REG_W16, seg, insn_eip);
                        emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)imm_val);
                        emit_sub_w(&code_ptr, REG_W10, REG_W16, REG_W14);
                        emit_flags_sub(&code_ptr, REG_W10, REG_W16, REG_W14);
                        last_cmp_type = CMP_NONE;
                    } else if (subop == 0 || subop == 1 || subop == 4 ||
                               subop == 5 || subop == 6) {
                        emit_str_w(&code_ptr, REG_W2, REG_SP, JIT_SLOT_EA);
                        emit_mem_load32(&code_ptr, REG_W16, seg, insn_eip);
                        emit_mov_imm32(&code_ptr, REG_W14, (uint32_t)imm_val);
                        switch (subop) {
                        case 0: /* ADD */
                            emit_add_w(&code_ptr, REG_W10, REG_W16, REG_W14);
                            emit_flags_add(&code_ptr, REG_W10, REG_W16, REG_W14);
                            break;
                        case 1: /* OR */
                            emit_orr_w(&code_ptr, REG_W10, REG_W16, REG_W14);
                            emit_flags_logic(&code_ptr, REG_W10);
                            break;
                        case 4: /* AND */
                            emit_and_w(&code_ptr, REG_W10, REG_W16, REG_W14);
                            emit_flags_logic(&code_ptr, REG_W10);
                            break;
                        case 5: /* SUB */
                            emit_sub_w(&code_ptr, REG_W10, REG_W16, REG_W14);
                            emit_flags_sub(&code_ptr, REG_W10, REG_W16, REG_W14);
                            break;
                        case 6: /* XOR */
                            emit_eor_w(&code_ptr, REG_W10, REG_W16, REG_W14);
                            emit_flags_logic(&code_ptr, REG_W10);
                            break;
                        }
                        emit_mov_w(&code_ptr, REG_W16, REG_W10);
                        emit_ldr_w(&code_ptr, REG_W2, REG_SP, JIT_SLOT_EA);
                        emit_mem_store32(&code_ptr, REG_W16, seg, insn_eip);
                        last_cmp_type = CMP_NONE;
                    } else {
                        goto fallback_exit;
                    }
                } else {
                    /* Read-only ALU with a memory source operand. Load r/m32
                     * into REG_W16 (which emit_flags_* never touch), then compute
                     * into the destination register via REG_W10 (the same temp
                     * the register form uses). CMP/TEST materialise flags here
                     * and clear last_cmp_type, so the mem temp need not survive
                     * past this instruction. */
                    emit_mem_load32(&code_ptr, REG_W16, seg, insn_eip);
                    switch (op) {
                    case 0x03: /* ADD r32, r/m32 */
                        emit_add_w(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_flags_add(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                        break;
                    case 0x2B: /* SUB r32, r/m32 */
                        emit_sub_w(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                        break;
                    case 0x23: /* AND r32, r/m32 */
                        emit_and_w(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_flags_logic(&code_ptr, REG_W10);
                        emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                        break;
                    case 0x0B: /* OR r32, r/m32 */
                        emit_orr_w(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_flags_logic(&code_ptr, REG_W10);
                        emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                        break;
                    case 0x33: /* XOR r32, r/m32 */
                        emit_eor_w(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_flags_logic(&code_ptr, REG_W10);
                        emit_mov_w(&code_ptr, x86_to_arm[reg], REG_W10);
                        break;
                    case 0x3B: /* CMP r32, r/m32 (read-only) */
                        emit_sub_w(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        emit_flags_sub(&code_ptr, REG_W10, x86_to_arm[reg], REG_W16);
                        break;
                    case 0x39: /* CMP r/m32, r32 (read-only) */
                        emit_sub_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_sub(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        break;
                    case 0x85: /* TEST r/m32, r32 (read-only) */
                        emit_and_w(&code_ptr, REG_W10, REG_W16, x86_to_arm[reg]);
                        emit_flags_logic(&code_ptr, REG_W10);
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

                uint32_t *branch_insn = code_ptr;

                if (prev_cmp == CMP_NONE) {
                    emit_ldr_w(&code_ptr, REG_W10, REG_X19, CPU_OFF_FLAGS);
                    emit_ubfx_w(&code_ptr, REG_W9, REG_W10, 6, 1); /* extract ZF (bit 6) */
                    if (op == 0x74) {
                        /* JZ: taken if ZF == 1 */
                        branch_insn = code_ptr;
                        emit_cbnz_w(&code_ptr, REG_W9, 0);
                    } else {
                        /* JNZ: taken if ZF == 0 */
                        branch_insn = code_ptr;
                        emit_cbz_w(&code_ptr, REG_W9, 0);
                    }
                } else {
                    if (prev_cmp == CMP_TEST) {
                        emit_cmp_imm_w(&code_ptr, REG_W10, 0);
                    } else if (prev_cmp == CMP_REG_REG) {
                        emit_cmp_w(&code_ptr, cmp_r1, cmp_r2);
                    } else {
                        emit_mov_imm32(&code_ptr, REG_W9, cmp_imm);
                        emit_cmp_w(&code_ptr, cmp_r1, REG_W9);
                    }
                    int cond = COND_EQ;
                    switch (op) {
                    case 0x74: cond = COND_EQ; break; /* JZ / JE */
                    case 0x75: cond = COND_NE; break; /* JNZ / JNE */
                    case 0x72: cond = COND_CC; break; /* JB / JC (unsigned <) */
                    case 0x73: cond = COND_CS; break; /* JAE / JNC (unsigned >=) */
                    case 0x77: cond = COND_HI; break; /* JA (unsigned >) */
                    case 0x76: cond = COND_LS; break; /* JBE (unsigned <=) */
                    }
                    branch_insn = code_ptr;
                    emit_b_cond(&code_ptr, cond, 0);
                }

                emit_block_tail(&code_ptr, next_eip, (uint32_t)(insn_count + 1),
                                cpu, blk_start_off, start_eip);
                uint32_t *taken_target = code_ptr;
                int32_t offset = (int32_t)((uint8_t *)taken_target - (uint8_t *)branch_insn);

                uint32_t *p = branch_insn;
                if (prev_cmp == CMP_NONE) {
                    if (op == 0x74) emit_cbnz_w(&p, REG_W9, offset);
                    else emit_cbz_w(&p, REG_W9, offset);
                } else {
                    int cond = COND_EQ;
                    switch (op) {
                    case 0x74: cond = COND_EQ; break;
                    case 0x75: cond = COND_NE; break;
                    case 0x72: cond = COND_CC; break;
                    case 0x73: cond = COND_CS; break;
                    case 0x77: cond = COND_HI; break;
                    case 0x76: cond = COND_LS; break;
                    }
                    emit_b_cond(&p, cond, offset);
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
                    emit_mul_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[reg2], x86_to_arm[rm2]);
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
                        emit_uxtb_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2]);
                    } else {
                        emit_ubfx_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2 - 4], 8, 8);
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
                    emit_uxth_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2]);
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
                        emit_sxtb_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2]);
                    } else {
                        emit_sbfx_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2 - 4], 8, 8);
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
                    emit_sxth_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2]);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BSWAP r32 (0x0F 0xC8 .. 0x0F 0xCF) */
            if (op2 >= 0xC8 && op2 <= 0xCF) {
                int rd = op2 - 0xC8;
                emit_rev_w(&code_ptr, x86_to_arm[rd], x86_to_arm[rd]);
                cur_eip += 2;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
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
                        emit_mov_imm32(&code_ptr, REG_W3, cnt);
                        emit_shxd(&code_ptr, x86_to_arm[rm2], x86_to_arm[reg2], REG_W3, is_shld);
                    } else {
                        /* CL form: mask to 5 bits first (a dst==ECX encoding
                         * must not corrupt the count), then skip if count==0 */
                        emit_ubfx_w(&code_ptr, REG_W0, x86_to_arm[1], 0, 5);
                        uint32_t *br = code_ptr;
                        emit_cbz_w(&code_ptr, REG_W0, 0);
                        emit_shxd(&code_ptr, x86_to_arm[rm2], x86_to_arm[reg2], REG_W0, is_shld);
                        emit_cbz_w(&br, REG_W0,
                                   (int32_t)((uint8_t *)code_ptr - (uint8_t *)br));
                    }
                    cur_eip += is_imm ? 4 : 3;
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
                    /* flags first (helpers read the untouched operands and
                     * ignore the res reg), then b = old a, a = res. */
                    emit_mov_w(&code_ptr, REG_W2, x86_to_arm[rm2]);       /* old a */
                    emit_add_w(&code_ptr, REG_W16, x86_to_arm[rm2], x86_to_arm[reg2]); /* res */
                    emit_flags_add(&code_ptr, REG_W16, REG_W2, x86_to_arm[reg2]);
                    emit_mov_w(&code_ptr, x86_to_arm[reg2], REG_W2);      /* b = old a */
                    emit_mov_w(&code_ptr, x86_to_arm[rm2], REG_W16);      /* a = res */
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
                    emit_mov_w(&code_ptr, REG_W2, x86_to_arm[rm2]);       /* src1 */
                    emit_sub_w(&code_ptr, REG_W16, REG_W2, x86_to_arm[0]); /* res */
                    emit_flags_sub(&code_ptr, REG_W16, REG_W2, x86_to_arm[0]);
                    uint32_t *br = code_ptr;
                    emit_cbnz_w(&code_ptr, REG_W16, 0);                   /* res != 0 -> else */
                    emit_mov_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[reg2]); /* equal: r/m = src */
                    uint32_t *jmpp = code_ptr;
                    emit_b_cond(&code_ptr, 0xE, 0);                       /* AL: skip else */
                    uint32_t *else_start = code_ptr;
                    emit_mov_w(&code_ptr, x86_to_arm[0], REG_W2);         /* else: EAX = src1 */
                    emit_cbnz_w(&br, REG_W16,
                                (int32_t)((uint8_t *)else_start - (uint8_t *)br));
                    emit_b_cond(&jmpp, 0xE, (int32_t)((uint8_t *)code_ptr - (uint8_t *)jmpp));
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BSF r32, r/m32 (0x0F 0xBC) */
            if (op2 == 0xBC) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_ctz_w(&code_ptr, x86_to_arm[reg2], x86_to_arm[rm2]);
                    emit_cmp_imm_w(&code_ptr, x86_to_arm[rm2], 0);
                    emit_cset_w(&code_ptr, REG_W10, COND_EQ); /* ZF = (src == 0) */
                    emit_flags_zf(&code_ptr, REG_W10);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BSR r32, r/m32 (0x0F 0xBD) */
            if (op2 == 0xBD) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_clz_w(&code_ptr, REG_W10, x86_to_arm[rm2]);
                    emit_mov_imm32(&code_ptr, REG_W9, 31);
                    emit_sub_w(&code_ptr, x86_to_arm[reg2], REG_W9, REG_W10);
                    emit_cmp_imm_w(&code_ptr, x86_to_arm[rm2], 0);
                    emit_cset_w(&code_ptr, REG_W10, COND_EQ); /* ZF = (src == 0) */
                    emit_flags_zf(&code_ptr, REG_W10);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BT r/m32, r32 (0x0F 0xA3) */
            if (op2 == 0xA3) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_lsrv_w(&code_ptr, REG_W10, x86_to_arm[rm2], x86_to_arm[reg2]);
                    emit_ubfx_w(&code_ptr, REG_W10, REG_W10, 0, 1);
                    emit_flags_cf(&code_ptr, REG_W10);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BTS r/m32, r32 (0x0F 0xAB) */
            if (op2 == 0xAB) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_lsrv_w(&code_ptr, REG_W10, x86_to_arm[rm2], x86_to_arm[reg2]);
                    emit_ubfx_w(&code_ptr, REG_W10, REG_W10, 0, 1);
                    emit_flags_cf(&code_ptr, REG_W10);
                    emit_mov_imm32(&code_ptr, REG_W9, 1);
                    emit_lslv_w(&code_ptr, REG_W9, REG_W9, x86_to_arm[reg2]);
                    emit_orr_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[rm2], REG_W9);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BTR r/m32, r32 (0x0F 0xB3) */
            if (op2 == 0xB3) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_lsrv_w(&code_ptr, REG_W10, x86_to_arm[rm2], x86_to_arm[reg2]);
                    emit_ubfx_w(&code_ptr, REG_W10, REG_W10, 0, 1);
                    emit_flags_cf(&code_ptr, REG_W10);
                    emit_mov_imm32(&code_ptr, REG_W9, 1);
                    emit_lslv_w(&code_ptr, REG_W9, REG_W9, x86_to_arm[reg2]);
                    emit_bic_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[rm2], REG_W9);
                    cur_eip += 3;
                    insn_count++;
                    last_cmp_type = CMP_NONE;
                    continue;
                }
            }

            /* BTC r/m32, r32 (0x0F 0xBB) */
            if (op2 == 0xBB) {
                uint8_t modrm2 = jit_read_u8(cpu, cur_eip + 2);
                uint8_t mod2 = (modrm2 >> 6) & 3;
                uint8_t reg2 = (modrm2 >> 3) & 7;
                uint8_t rm2  = modrm2 & 7;
                if (mod2 == 3) {
                    emit_lsrv_w(&code_ptr, REG_W10, x86_to_arm[rm2], x86_to_arm[reg2]);
                    emit_ubfx_w(&code_ptr, REG_W10, REG_W10, 0, 1);
                    emit_flags_cf(&code_ptr, REG_W10);
                    emit_mov_imm32(&code_ptr, REG_W9, 1);
                    emit_lslv_w(&code_ptr, REG_W9, REG_W9, x86_to_arm[reg2]);
                    emit_eor_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[rm2], REG_W9);
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
                    emit_ubfx_w(&code_ptr, REG_W10, x86_to_arm[rm2], imm8, 1);
                    emit_flags_cf(&code_ptr, REG_W10);
                    if (subop == 5) {
                        emit_mov_imm32(&code_ptr, REG_W9, 1u << imm8);
                        emit_orr_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[rm2], REG_W9);
                    } else if (subop == 6) {
                        emit_mov_imm32(&code_ptr, REG_W9, 1u << imm8);
                        emit_bic_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[rm2], REG_W9);
                    } else if (subop == 7) {
                        emit_mov_imm32(&code_ptr, REG_W9, 1u << imm8);
                        emit_eor_w(&code_ptr, x86_to_arm[rm2], x86_to_arm[rm2], REG_W9);
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
                    /* Load cpu->flags (offset 40) into REG_W13 */
                    emit_ldr_w(&code_ptr, REG_W13, REG_X19, CPU_OFF_FLAGS);
                    /* Extract condition flags */
                    emit_ubfx_w(&code_ptr, REG_W9, REG_W13, 0, 1);   /* CF (bit 0) */
                    emit_ubfx_w(&code_ptr, REG_W10, REG_W13, 6, 1);  /* ZF (bit 6) */
                    emit_ubfx_w(&code_ptr, REG_W11, REG_W13, 7, 1);  /* SF (bit 7) */
                    emit_ubfx_w(&code_ptr, REG_W12, REG_W13, 11, 1); /* OF (bit 11) */

                    switch (cc) {
                    case 0x0: /* SETO (OF == 1) */
                        emit_mov_w(&code_ptr, REG_W14, REG_W12);
                        break;
                    case 0x1: /* SETNO (OF == 0) */
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W12, REG_W15);
                        break;
                    case 0x2: /* SETB / SETC (CF == 1) */
                        emit_mov_w(&code_ptr, REG_W14, REG_W9);
                        break;
                    case 0x3: /* SETAE / SETNC (CF == 0) */
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W9, REG_W15);
                        break;
                    case 0x4: /* SETZ / SETE (ZF == 1) */
                        emit_mov_w(&code_ptr, REG_W14, REG_W10);
                        break;
                    case 0x5: /* SETNZ / SETNE (ZF == 0) */
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W10, REG_W15);
                        break;
                    case 0x6: /* SETBE (CF == 1 || ZF == 1) */
                        emit_orr_w(&code_ptr, REG_W14, REG_W9, REG_W10);
                        break;
                    case 0x7: /* SETA (CF == 0 && ZF == 0) */
                        emit_orr_w(&code_ptr, REG_W14, REG_W9, REG_W10);
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W14, REG_W15);
                        break;
                    case 0x8: /* SETS (SF == 1) */
                        emit_mov_w(&code_ptr, REG_W14, REG_W11);
                        break;
                    case 0x9: /* SETNS (SF == 0) */
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W11, REG_W15);
                        break;
                    case 0xC: /* SETL (SF != OF) */
                        emit_eor_w(&code_ptr, REG_W14, REG_W11, REG_W12);
                        break;
                    case 0xD: /* SETGE (SF == OF) */
                        emit_eor_w(&code_ptr, REG_W14, REG_W11, REG_W12);
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W14, REG_W15);
                        break;
                    case 0xE: /* SETLE (ZF == 1 || SF != OF) */
                        emit_eor_w(&code_ptr, REG_W14, REG_W11, REG_W12);
                        emit_orr_w(&code_ptr, REG_W14, REG_W14, REG_W10);
                        break;
                    case 0xF: /* SETG (ZF == 0 && SF == OF) */
                        emit_eor_w(&code_ptr, REG_W14, REG_W11, REG_W12);
                        emit_orr_w(&code_ptr, REG_W14, REG_W14, REG_W10);
                        emit_mov_imm32(&code_ptr, REG_W15, 1);
                        emit_eor_w(&code_ptr, REG_W14, REG_W14, REG_W15);
                        break;
                    default:
                        goto fallback_exit;
                    }

                    emit_write_r8(&code_ptr, rm2, REG_W14);
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

                    uint32_t *branch_insn = code_ptr;

                    if (prev_cmp == CMP_NONE) {
                        emit_ldr_w(&code_ptr, REG_W10, REG_X19, CPU_OFF_FLAGS);
                        emit_ubfx_w(&code_ptr, REG_W9, REG_W10, 6, 1); /* ZF (bit 6) */
                        if (op2 == 0x84) {
                            branch_insn = code_ptr;
                            emit_cbnz_w(&code_ptr, REG_W9, 0);
                        } else {
                            branch_insn = code_ptr;
                            emit_cbz_w(&code_ptr, REG_W9, 0);
                        }
                    } else {
                        if (prev_cmp == CMP_TEST) {
                            emit_cmp_imm_w(&code_ptr, REG_W10, 0);
                        } else if (prev_cmp == CMP_REG_REG) {
                            emit_cmp_w(&code_ptr, cmp_r1, cmp_r2);
                        } else {
                            emit_mov_imm32(&code_ptr, REG_W9, cmp_imm);
                            emit_cmp_w(&code_ptr, cmp_r1, REG_W9);
                        }
                        int cond = COND_EQ;
                        switch (op2) {
                        case 0x84: cond = COND_EQ; break;
                        case 0x85: cond = COND_NE; break;
                        case 0x82: cond = COND_CC; break;
                        case 0x83: cond = COND_CS; break;
                        case 0x87: cond = COND_HI; break;
                        case 0x86: cond = COND_LS; break;
                        }
                        branch_insn = code_ptr;
                        emit_b_cond(&code_ptr, cond, 0);
                    }

                    emit_block_tail(&code_ptr, next_eip, (uint32_t)(insn_count + 1),
                                    cpu, blk_start_off, start_eip);
                    uint32_t *taken_target = code_ptr;
                    int32_t offset = (int32_t)((uint8_t *)taken_target - (uint8_t *)branch_insn);

                    uint32_t *p = branch_insn;
                    if (prev_cmp == CMP_NONE) {
                        if (op2 == 0x84) emit_cbnz_w(&p, REG_W9, offset);
                        else emit_cbz_w(&p, REG_W9, offset);
                    } else {
                        int cond = COND_EQ;
                        switch (op2) {
                        case 0x84: cond = COND_EQ; break;
                        case 0x85: cond = COND_NE; break;
                        case 0x82: cond = COND_CC; break;
                        case 0x83: cond = COND_CS; break;
                        case 0x87: cond = COND_HI; break;
                        case 0x86: cond = COND_LS; break;
                        }
                        emit_b_cond(&p, cond, offset);
                    }

                    emit_block_tail(&code_ptr, target_eip, (uint32_t)(insn_count + 1),
                                    cpu, blk_start_off, start_eip);
                    insn_count++;
                    goto block_finished;
                }
            }
        }

    fallback_exit:
        break;
    }

    if (insn_count > 0) {
        goto block_done;
    }

    jit_write_disable();
    { bool ok; uint32_t key = jit_key_of(cpu, start_eip, &ok); (void)key;
      if (ok) jit_insert_block(cpu, start_eip, NULL, 0); }
    return NULL;

block_done:
    emit_block_tail(&code_ptr, cur_eip, (uint32_t)insn_count,
                    cpu, blk_start_off, start_eip);

block_finished:
    {
        size_t block_bytes = (uint8_t *)code_ptr - (uint8_t *)code_start;
        s_jit_off += (block_bytes + 15) & ~15;

        jit_sync_range(code_start, block_bytes);
        jit_write_disable();

        { bool ok; uint32_t key = jit_key_of(cpu, start_eip, &ok); (void)key;
          if (ok) { jit_mark_code_pages(cpu, start_eip, cur_eip > start_eip ? cur_eip : start_eip + 1);
                    jit_insert_block(cpu, start_eip, (void *)code_start, (insn_count > 0) ? insn_count : 1); } }
#ifdef DEBUG_JIT_COMPILE
        fprintf(stderr, "[JIT COMPILE #%u] pc=%08x..%08x (len=%d insns)\n", s_jit_compile_count, start_eip, cur_eip, insn_count);
#endif
        s_jit_compile_count++;

        return (void *)code_start;
    }
}
