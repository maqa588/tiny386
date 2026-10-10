#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* --- Linux ABI Syscall Stubs for QEMU-RISCV32 --- */
#ifdef __riscv
static void raw_write_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    register int a0 asm("a0") = 1;
    register const char *a1 asm("a1") = s;
    register size_t a2 asm("a2") = len;
    register int a7 asm("a7") = 64; /* __NR_write */
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

static void raw_write_hex(uint32_t val) {
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    const char *hex = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[10] = '\0';
    raw_write_str(buf);
}

static void raw_write_dec(uint32_t val) {
    char buf[16];
    int i = 14;
    buf[15] = '\0';
    if (val == 0) {
        raw_write_str("0");
        return;
    }
    while (val > 0 && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    raw_write_str(&buf[i + 1]);
}

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, int32_t offset) {
    register void *a0 asm("a0") = addr;
    register size_t a1 asm("a1") = len;
    register int a2 asm("a2") = prot;
    register int a3 asm("a3") = flags;
    register int a4 asm("a4") = fd;
    register int32_t a5 asm("a5") = offset;
    register int a7 asm("a7") = 222; /* __NR_mmap */
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}

void _exit(int status) {
    register int a0 asm("a0") = status;
    register int a7 asm("a7") = 93; /* __NR_exit */
    asm volatile ("ecall" : : "r"(a0), "r"(a7) : "memory");
    while (1);
}
#endif
/* ---------------------------------------------------------------------------------- */

/**
 * @brief Simulated x86 CPU Context Structure
 */
struct x86_cpu_state {
    uint32_t eax;   /* offset 0  */
    uint32_t ecx;   /* offset 4  */
    uint32_t edx;   /* offset 8  */
    uint32_t ebx;   /* offset 12 */
    uint32_t esp;   /* offset 16 */
    uint32_t ebp;   /* offset 20 */
    uint32_t esi;   /* offset 24 */
    uint32_t edi;   /* offset 28 */
    uint32_t eip;   /* offset 32 */
    uint32_t flags; /* offset 36 */
};

/* RISC-V 32 Standard Registers */
enum {
    REG_ZERO = 0,
    REG_RA   = 1,
    REG_SP   = 2,
    REG_GP   = 3,
    REG_TP   = 4,
    REG_T0   = 5,
    REG_T1   = 6,
    REG_T2   = 7,
    REG_S0   = 8,   /* Mapped to virtual CPU context struct pointer */
    REG_S1   = 9,   /* Mapped to x86 EAX */
    REG_A0   = 10,
    REG_A1   = 11,
    REG_A2   = 12,
    REG_A3   = 13,
    REG_A4   = 14,
    REG_A5   = 15,
    REG_A6   = 16,
    REG_A7   = 17,
    REG_S2   = 18,  /* Mapped to x86 ECX */
    REG_S3   = 19,  /* Mapped to x86 EDX */
    REG_S4   = 20,  /* Mapped to x86 EBX */
    REG_S5   = 21,  /* Mapped to x86 ESP */
    REG_S6   = 22,  /* Mapped to x86 EBP */
    REG_S7   = 23,  /* Mapped to x86 ESI */
    REG_S8   = 24,  /* Mapped to x86 EDI */
    REG_S9   = 25,  /* Mapped to x86 EIP */
};

/**
 * @brief RV32 Instruction Emitter Helper Functions
 */
static inline void emit32(uint32_t **buf, uint32_t insn)
{
    **buf = insn;
    (*buf)++;
}

/* R-type: opcode=0x33 */
static inline void emit_add(uint32_t **buf, int rd, int rs1, int rs2)
{
    emit32(buf, 0x33 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

static inline void emit_sub(uint32_t **buf, int rd, int rs1, int rs2)
{
    emit32(buf, 0x33 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | (rs2 << 20) | (0x20 << 25));
}

static inline void emit_xor(uint32_t **buf, int rd, int rs1, int rs2)
{
    emit32(buf, 0x33 | (rd << 7) | (0x4 << 12) | (rs1 << 15) | (rs2 << 20) | (0x00 << 25));
}

/* I-type: opcode=0x13 */
static inline void emit_addi(uint32_t **buf, int rd, int rs1, int32_t imm)
{
    emit32(buf, 0x13 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | ((imm & 0xfff) << 20));
}

/* I-type Load: opcode=0x03, funct3=0x2 (LW) */
static inline void emit_lw(uint32_t **buf, int rd, int rs1, int32_t offset)
{
    emit32(buf, 0x03 | (rd << 7) | (0x2 << 12) | (rs1 << 15) | ((offset & 0xfff) << 20));
}

/* S-type Store: opcode=0x23, funct3=0x2 (SW) */
static inline void emit_sw(uint32_t **buf, int rs2, int rs1, int32_t offset)
{
    uint32_t imm = (uint32_t)offset & 0xfff;
    emit32(buf, 0x23 | ((imm & 0x1f) << 7) | (0x2 << 12) | (rs1 << 15) | (rs2 << 20) | (((imm >> 5) & 0x7f) << 25));
}

/* I-type JALR: opcode=0x67, funct3=0x0 */
static inline void emit_jalr(uint32_t **buf, int rd, int rs1, int32_t offset)
{
    emit32(buf, 0x67 | (rd << 7) | (0x0 << 12) | (rs1 << 15) | ((offset & 0xfff) << 20));
}

static inline void emit_ret(uint32_t **buf)
{
    emit_jalr(buf, REG_ZERO, REG_RA, 0);
}

typedef void (*jit_func_t)(struct x86_cpu_state *cpu);

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    raw_write_str("============================================================\n");
    raw_write_str("       Tiny386 - RV32 Lightweight JIT Prototype Test        \n");
    raw_write_str("============================================================\n");

    /* 1. Allocate an executable page for JIT buffer with PROT_READ|PROT_WRITE|PROT_EXEC */
    size_t code_buf_size = 4096;
    void *code_mem = my_mmap(NULL, code_buf_size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if ((intptr_t)code_mem < 0) {
        raw_write_str("[-] Error: my_mmap failed!\n");
        return 1;
    }

    uint32_t *code_ptr = (uint32_t *)code_mem;
    uint32_t *start_ptr = code_ptr;

    raw_write_str("[1] JIT Executable Buffer allocated at: ");
    raw_write_hex((uint32_t)(uintptr_t)code_mem);
    raw_write_str(" (size: ");
    raw_write_dec((uint32_t)code_buf_size);
    raw_write_str(" bytes)\n");

    /* 
     * 2. Build JIT Function:
     *
     * ABI Calling Convention:
     *   Argument: a0 = struct x86_cpu_state *cpu
     *
     * Prologue:
     *   addi sp, sp, -32
     *   sw   ra, 28(sp)
     *   sw   s0, 24(sp)
     *   sw   s1, 20(sp)
     *   sw   s2, 16(sp)
     *   sw   s3, 12(sp)
     *   addi s0, a0, 0          # s0 = cpu pointer
     */
    emit_addi(&code_ptr, REG_SP, REG_SP, -32);
    emit_sw(&code_ptr, REG_RA, REG_SP, 28);
    emit_sw(&code_ptr, REG_S0, REG_SP, 24);
    emit_sw(&code_ptr, REG_S1, REG_SP, 20);
    emit_sw(&code_ptr, REG_S2, REG_SP, 16);
    emit_sw(&code_ptr, REG_S3, REG_SP, 12);
    emit_addi(&code_ptr, REG_S0, REG_A0, 0);

    /* 
     * Load simulated x86 registers into dedicated RV32 callee-saved registers:
     *   lw s1, offsetof(eax)(s0)
     *   lw s2, offsetof(ecx)(s0)
     *   lw s3, offsetof(edx)(s0)
     */
    emit_lw(&code_ptr, REG_S1, REG_S0, offsetof(struct x86_cpu_state, eax));
    emit_lw(&code_ptr, REG_S2, REG_S0, offsetof(struct x86_cpu_state, ecx));
    emit_lw(&code_ptr, REG_S3, REG_S0, offsetof(struct x86_cpu_state, edx));

    /*
     * Translate simulated x86 instructions (Micro-ops):
     * 1. add eax, ecx  ==>  add s1, s1, s2
     * 2. sub eax, edx  ==>  sub s1, s1, s3
     * 3. add eax, 5    ==>  addi s1, s1, 5
     */
    emit_add(&code_ptr, REG_S1, REG_S1, REG_S2);
    emit_sub(&code_ptr, REG_S1, REG_S1, REG_S3);
    emit_addi(&code_ptr, REG_S1, REG_S1, 5);

    /*
     * Write back results:
     *   sw s1, offsetof(eax)(s0)
     */
    emit_sw(&code_ptr, REG_S1, REG_S0, offsetof(struct x86_cpu_state, eax));

    /*
     * Epilogue:
     *   lw   ra, 28(sp)
     *   lw   s0, 24(sp)
     *   lw   s1, 20(sp)
     *   lw   s2, 16(sp)
     *   lw   s3, 12(sp)
     *   addi sp, sp, 32
     *   ret (jalr zero, 0(ra))
     */
    emit_lw(&code_ptr, REG_RA, REG_SP, 28);
    emit_lw(&code_ptr, REG_S0, REG_SP, 24);
    emit_lw(&code_ptr, REG_S1, REG_SP, 20);
    emit_lw(&code_ptr, REG_S2, REG_SP, 16);
    emit_lw(&code_ptr, REG_S3, REG_SP, 12);
    emit_addi(&code_ptr, REG_SP, REG_SP, 32);
    emit_ret(&code_ptr);

    size_t insn_count = (size_t)(code_ptr - start_ptr);
    raw_write_str("[2] Emitted ");
    raw_write_dec((uint32_t)insn_count);
    raw_write_str(" RV32 instructions (");
    raw_write_dec((uint32_t)(insn_count * 4));
    raw_write_str(" bytes):\n");
    for (size_t i = 0; i < insn_count; i++) {
        raw_write_str("    [insn] ");
        raw_write_hex(start_ptr[i]);
        raw_write_str("\n");
    }

    /* 3. Flush Instruction Cache via fence.i */
    raw_write_str("[3] Synchronizing I-Cache via fence.i...\n");
    asm volatile (".insn i 0x0f, 0x1, x0, x0, 0" ::: "memory");

    /* 4. Prepare Test State */
    struct x86_cpu_state cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.eax = 0x1000; /* 4096 */
    cpu.ecx = 0x0337; /* 823  */
    cpu.edx = 0x0020; /* 32   */

    raw_write_str("[4] Initial x86 CPU Context:\n");
    raw_write_str("    EAX: "); raw_write_hex(cpu.eax); raw_write_str(" ("); raw_write_dec(cpu.eax); raw_write_str(")\n");
    raw_write_str("    ECX: "); raw_write_hex(cpu.ecx); raw_write_str(" ("); raw_write_dec(cpu.ecx); raw_write_str(")\n");
    raw_write_str("    EDX: "); raw_write_hex(cpu.edx); raw_write_str(" ("); raw_write_dec(cpu.edx); raw_write_str(")\n");
    raw_write_str("    Formula: EAX = EAX + ECX - EDX + 5\n");
    uint32_t expected = 0x1000 + 0x0337 - 0x0020 + 5;
    raw_write_str("    Expected: "); raw_write_hex(expected); raw_write_str(" ("); raw_write_dec(expected); raw_write_str(")\n");

    /* 5. Execute JIT Function */
    raw_write_str("[5] Invoking JIT Compiled Function via function pointer...\n");
    jit_func_t jit_entry = (jit_func_t)start_ptr;
    jit_entry(&cpu);

    /* 6. Verify Result */
    raw_write_str("[6] CPU Context after JIT Execution:\n");
    raw_write_str("    EAX: "); raw_write_hex(cpu.eax); raw_write_str(" ("); raw_write_dec(cpu.eax); raw_write_str(")\n");

    if (cpu.eax == expected) {
        raw_write_str("============================================================\n");
        raw_write_str("  >>> SUCCESS: RV32 JIT Template Execution Verified! <<<\n");
        raw_write_str("============================================================\n");
        return 0;
    } else {
        raw_write_str(">>> ERROR: Expected "); raw_write_hex(expected);
        raw_write_str(" but got "); raw_write_hex(cpu.eax); raw_write_str("\n");
        return 1;
    }
}
