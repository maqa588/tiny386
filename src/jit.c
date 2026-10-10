#include "jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BUILD_ESP32
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_attr.h"
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

/* ESP32-P4 768KB HP L2 SRAM: budget 512KB for emulator core (384KB Code Cache + 96KB Hash + 8KB Hotspot + 16KB TLB + 4KB CPU) */
#ifdef BUILD_ESP32
#define JIT_BUF_SIZE        (384 * 1024)    /* 384KB fast executable buffer in internal HP L2 SRAM */
#define JIT_HASH_SETS       4096            /* 4K sets */
#else
/* Host build (e.g. qemu-riscv32): RAM is plentiful, so keep the whole hot set
   resident. A small cache would constantly flush and recompile Windows 95's
   working set, which is far slower than having a larger buffer. */
#define JIT_BUF_SIZE        (16 * 1024 * 1024)  /* 16MB executable buffer */
#define JIT_HASH_SETS       16384               /* 16K sets */
#endif
#define JIT_HASH_WAYS       2               /* 2-way set associative */
#define JIT_HOTSPOT_SIZE    8192            /* 8K hotspot counters (8KB) */
#define JIT_HOTSPOT_THRESHOLD 16            /* Trigger compile on 16 executions */

typedef struct {
    uint32_t guest_pc;
    void *host_fn;
    uint32_t num_insns;
} JITEntry;

static uint8_t *s_jit_buf = NULL;
static size_t s_jit_off = 0;
static JITEntry (*s_hash_tab)[JIT_HASH_WAYS] = NULL;
static uint8_t *s_hash_lru = NULL;
static uint8_t *s_hotspot = NULL;
static bool s_initialized = false;
static bool s_jit_enabled = true;
static int s_jit_ring0_allowed = 0;
static uint32_t s_jit_exec_count = 0;
static uint32_t s_jit_compile_count = 0;

/* External helpers declared in i386.h */
extern bool cpu_load8(CPUI386 *cpu, int seg, uint32_t addr, uint8_t *res);
extern bool cpu_load32(CPUI386 *cpu, int seg, uint32_t addr, uint32_t *res);
extern bool cpu_store32(CPUI386 *cpu, int seg, uint32_t addr, uint32_t val);

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

static inline void emit_jalr(uint32_t **buf, int rd, int rs1, int32_t offset12) {
    emit32(buf, 0x67 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | ((offset12 & 0xfff) << 20));
}

static inline void emit_ret(uint32_t **buf) {
    emit_jalr(buf, REG_ZERO, REG_RA, 0);
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

void jit_enable(bool enable) {
    s_jit_enabled = enable;
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

static inline uint32_t jit_hash(uint32_t pc) {
    return (pc * 2654435761u) & (JIT_HASH_SETS - 1);
}

uint32_t jit_get_block_insn_count(uint32_t guest_pc) {
    if (!s_initialized || !s_hash_tab) return 1;
    uint32_t set = jit_hash(guest_pc);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        if (s_hash_tab[set][way].guest_pc == guest_pc && s_hash_tab[set][way].num_insns > 0) {
            return s_hash_tab[set][way].num_insns;
        }
    }
    return 1;
}

/* Side-effect-free check whether a compiled block already exists for guest_pc.
   Used during compilation to avoid inlining already-translated targets. */
static bool jit_has_block(uint32_t guest_pc) {
    if (!s_initialized || !s_hash_tab) return false;
    uint32_t set = jit_hash(guest_pc);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        if (s_hash_tab[set][way].guest_pc == guest_pc && s_hash_tab[set][way].host_fn != NULL) {
            return true;
        }
    }
    return false;
}

void jit_flush_cache(void) {
#if defined(__riscv)
    asm volatile (".insn i 0x0f, 0x1, x0, x0, 0" ::: "memory");
#endif
#ifdef BUILD_ESP32
    if (s_jit_buf) {
        esp_cache_msync(s_jit_buf, s_jit_off, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
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
#ifdef MALLOC_CAP_EXEC
    s_jit_buf = heap_caps_malloc(JIT_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_EXEC);
    if (!s_jit_buf)
        s_jit_buf = heap_caps_malloc(256 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_EXEC);
    if (!s_jit_buf)
        s_jit_buf = heap_caps_malloc(JIT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_EXEC);
#else
    s_jit_buf = heap_caps_malloc(JIT_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_jit_buf)
        s_jit_buf = heap_caps_malloc(256 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_jit_buf)
        s_jit_buf = heap_caps_malloc(JIT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_jit_buf) {
        s_jit_buf = malloc(JIT_BUF_SIZE);
    }
    printf("[JIT] Code Buffer allocated at %p (%u KB) [Internal L2 SRAM: %s]\n",
           s_jit_buf, (unsigned)(JIT_BUF_SIZE / 1024), esp_ptr_internal(s_jit_buf) ? "YES" : "NO");
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
    }
    if (!s_hash_lru) {
        s_hash_lru = (uint8_t *)heap_caps_calloc(JIT_HASH_SETS, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_hash_lru) s_hash_lru = (uint8_t *)heap_caps_calloc(JIT_HASH_SETS, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hash_lru) s_hash_lru = (uint8_t *)calloc(JIT_HASH_SETS, 1);
    }
    if (!s_hotspot) {
        s_hotspot = (uint8_t *)heap_caps_calloc(JIT_HOTSPOT_SIZE, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_hotspot) s_hotspot = (uint8_t *)heap_caps_calloc(JIT_HOTSPOT_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hotspot) s_hotspot = (uint8_t *)calloc(JIT_HOTSPOT_SIZE, 1);
    }
#else
    if (!s_hash_tab) s_hash_tab = (JITEntry (*)[JIT_HASH_WAYS])calloc(JIT_HASH_SETS * JIT_HASH_WAYS, sizeof(JITEntry));
    if (!s_hash_lru) s_hash_lru = (uint8_t *)calloc(JIT_HASH_SETS, 1);
    if (!s_hotspot) s_hotspot = (uint8_t *)calloc(JIT_HOTSPOT_SIZE, 1);
#endif

    s_jit_off = 0;
    if (s_hash_tab) memset(s_hash_tab, 0, JIT_HASH_SETS * JIT_HASH_WAYS * sizeof(JITEntry));
    if (s_hash_lru) memset(s_hash_lru, 0, JIT_HASH_SETS);
    if (s_hotspot) memset(s_hotspot, 0, JIT_HOTSPOT_SIZE);
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
    if (s_hotspot) {
        memset(s_hotspot, 0, JIT_HOTSPOT_SIZE);
    }
    jit_flush_cache();
}

void *jit_lookup(uint32_t guest_pc) {
    if (!s_jit_enabled || !s_initialized || !s_hash_tab) return NULL;
    uint32_t set = jit_hash(guest_pc);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        if (s_hash_tab[set][way].guest_pc == guest_pc && s_hash_tab[set][way].host_fn != NULL) {
            s_jit_exec_count++;
            if (s_hash_lru) s_hash_lru[set] = 1 - way;
            return s_hash_tab[set][way].host_fn;
        }
    }
    return NULL;
}

void *jit_lookup_ex(uint32_t guest_pc, uint32_t *num_insns) {
    if (num_insns) *num_insns = 1;
    if (!s_jit_enabled || !s_initialized || !s_hash_tab) return NULL;
    uint32_t set = jit_hash(guest_pc);
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        if (s_hash_tab[set][way].guest_pc == guest_pc && s_hash_tab[set][way].host_fn != NULL) {
            s_jit_exec_count++;
            if (s_hash_lru) s_hash_lru[set] = 1 - way;
            if (num_insns) {
                uint32_t n = s_hash_tab[set][way].num_insns;
                *num_insns = (n > 0) ? n : 1;
            }
            return s_hash_tab[set][way].host_fn;
        }
    }
    return NULL;
}

static void jit_insert_block(uint32_t guest_pc, void *host_fn, uint32_t num_insns) {
    if (!s_hash_tab) return;
    uint32_t set = jit_hash(guest_pc);
    int target_way = -1;

    /* 1. Check if guest_pc already present in this set */
    for (int way = 0; way < JIT_HASH_WAYS; way++) {
        if (s_hash_tab[set][way].guest_pc == guest_pc) {
            target_way = way;
            break;
        }
    }
    /* 2. Otherwise find an empty slot (host_fn == NULL and guest_pc == 0) */
    if (target_way < 0) {
        for (int way = 0; way < JIT_HASH_WAYS; way++) {
            if (s_hash_tab[set][way].guest_pc == 0 && s_hash_tab[set][way].host_fn == NULL) {
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
    s_hash_tab[set][target_way].host_fn = host_fn;
    s_hash_tab[set][target_way].num_insns = num_insns;
    if (s_hash_lru != NULL) {
        s_hash_lru[set] = 1 - target_way;
    }
}

bool jit_should_compile(uint32_t guest_pc) {
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
    uint32_t set = jit_hash(guest_pc);
    if (s_hash_tab) {
        for (int way = 0; way < JIT_HASH_WAYS; way++) {
            if (s_hash_tab[set][way].guest_pc == guest_pc) {
                return false;
            }
        }
    }
    uint32_t idx = (guest_pc) & (JIT_HOTSPOT_SIZE - 1);
    if (s_hotspot[idx] < 255) {
        s_hotspot[idx]++;
    }
    return (s_hotspot[idx] >= JIT_HOTSPOT_THRESHOLD);
}

/* --- Code Generator for Entry/Exit Contract --- */
static void emit_prologue(uint32_t **buf) {
    emit_addi(buf, REG_SP, REG_SP, -48);
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

    /* Load x86 GPRs: gprx[0..7] */
    for (int i = 0; i < 8; i++) {
        emit_lw(buf, x86_to_rv[i], REG_S0, i * 4);
    }
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
    emit_addi(buf, REG_SP, REG_SP, 48);

    emit_ret(buf);
}

static void emit_epilogue_reg(uint32_t **buf, int rv_reg_eip) {
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
    emit_addi(buf, REG_SP, REG_SP, 48);

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

    emit_prologue(&code_ptr);

    uint32_t cur_eip = start_eip;
    int insn_count = 0;
    int max_insns = 32;
    uint8_t last_op = 0;

    int last_cmp_type = CMP_NONE;
    int cmp_r1 = 0, cmp_r2 = 0;
    uint32_t cmp_imm = 0;

    while (insn_count < max_insns) {
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
            emit_addi(&code_ptr, REG_A3, REG_SP, 4);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_lw(&code_ptr, x86_to_rv[rd], REG_SP, 4);
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
            emit_addi(&code_ptr, REG_A3, REG_SP, 4);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_lw(&code_ptr, REG_T1, REG_SP, 4);
            emit_addi(&code_ptr, REG_S5, REG_S5, 4);
            insn_count++;
            emit_epilogue_reg(&code_ptr, REG_T1);
            goto block_finished;
        }

        /* RET imm16 (0xC2) */
        if (op == 0xC2) {
            uint16_t imm16 = jit_read_u8(cpu, cur_eip + 1) | (jit_read_u8(cpu, cur_eip + 2) << 8);
            uint32_t insn_eip = cur_eip;
            emit_addi(&code_ptr, REG_A0, REG_S0, 0);
            emit_addi(&code_ptr, REG_A1, REG_ZERO, 2 /* SEG_SS */);
            emit_addi(&code_ptr, REG_A2, REG_S5, 0);
            emit_addi(&code_ptr, REG_A3, REG_SP, 4);
            emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
            emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

            emit_mem_fault_check(&code_ptr, insn_eip);
            emit_lw(&code_ptr, REG_T1, REG_SP, 4);
            emit_addi(&code_ptr, REG_S5, REG_S5, 4 + imm16);
            insn_count++;
            emit_epilogue_reg(&code_ptr, REG_T1);
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

        /* 2-byte Opcode with ModR/M */
        uint8_t modrm = jit_read_u8(cpu, cur_eip + 1);
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t reg = (modrm >> 3) & 7;
        uint8_t rm  = modrm & 7;

        /* Register-to-Register operations (mod == 3) */
        if (mod == 3) {
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
        }

        /* Memory operations (mod < 3) */
        if ((op == 0x89 || op == 0x8B) && mod < 3) {
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

            if (valid_mem) {
                uint32_t insn_eip = cur_eip;

                /* Compute effective address into REG_A2 */
                if (has_sib && sib_index != 4) {
                    int index_rv = x86_to_rv[sib_index];
                    if (sib_scale == 0) {
                        emit_addi(&code_ptr, REG_A2, index_rv, 0);
                    } else {
                        emit_slli(&code_ptr, REG_A2, index_rv, sib_scale);
                    }
                    if (base_rv != -1) {
                        emit_add(&code_ptr, REG_A2, REG_A2, base_rv);
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

                if (op == 0x8B) {
                    /* Load: cpu_load32(cpu, seg, addr, SP + 4) */
                    emit_addi(&code_ptr, REG_A0, REG_S0, 0);
                    emit_addi(&code_ptr, REG_A1, REG_ZERO, seg);
                    emit_addi(&code_ptr, REG_A3, REG_SP, 4);
                    emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_load32);
                    emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

                    emit_mem_fault_check(&code_ptr, insn_eip);
                    emit_lw(&code_ptr, x86_to_rv[reg], REG_SP, 4);
                } else {
                    /* Store: cpu_store32(cpu, seg, addr, val) */
                    emit_addi(&code_ptr, REG_A0, REG_S0, 0);
                    emit_addi(&code_ptr, REG_A1, REG_ZERO, seg);
                    emit_addi(&code_ptr, REG_A3, x86_to_rv[reg], 0);
                    emit_mov_imm32(&code_ptr, REG_T0, (uint32_t)cpu_store32);
                    emit_jalr(&code_ptr, REG_RA, REG_T0, 0);

                    emit_mem_fault_check(&code_ptr, insn_eip);
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
            /* Merge a forward jump into this block (compile-through) so that
               straight-line stretches separated by unconditional jumps become a
               single block, avoiding prologue/epilogue + dispatch per fragment.
               Backward jumps and already-compiled targets end the block. */
            if (target > cur_eip && !jit_has_block(target)) {
                cur_eip = target;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
            cur_eip = target;
            insn_count++;
            goto block_done;
        }

        /* JMP rel32 (0xE9) */
        if (op == 0xE9) {
            int32_t rel32 = (int32_t)jit_read_u32(cpu, cur_eip + 1);
            uint32_t target = cur_eip + 5 + rel32;
            if (target > cur_eip && !jit_has_block(target)) {
                cur_eip = target;
                insn_count++;
                last_cmp_type = CMP_NONE;
                continue;
            }
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
                emit_epilogue(&code_ptr, next_eip);
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

                emit_epilogue(&code_ptr, target_eip);
                insn_count++;
                goto block_finished;
            }
        }

        /* 2-byte Near Conditional Jumps: 0x0F 0x80 .. 0x0F 0x8F */
        if (op == 0x0F) {
            uint8_t op2 = jit_read_u8(cpu, cur_eip + 1);
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
                    emit_epilogue(&code_ptr, next_eip);
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

                    emit_epilogue(&code_ptr, target_eip);
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
    jit_insert_block(start_eip, NULL, 0);
    return NULL;

block_done:
    emit_epilogue(&code_ptr, cur_eip);

block_finished:
    size_t block_bytes = (uint8_t *)code_ptr - (uint8_t *)code_start;
    s_jit_off += (block_bytes + 15) & ~15;

    jit_flush_cache();

    jit_insert_block(start_eip, (void *)code_start, (insn_count > 0) ? insn_count : 1);
#ifdef DEBUG_JIT_COMPILE
    fprintf(stderr, "[JIT COMPILE #%u] pc=%08x..%08x (len=%d insns)\n", s_jit_compile_count, start_eip, cur_eip, insn_count);
#endif
    s_jit_compile_count++;

    return (void *)code_start;
}
