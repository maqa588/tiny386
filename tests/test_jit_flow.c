#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

void _exit(int status) {
    register int a0 asm("a0") = status;
    register int a7 asm("a7") = 93; /* __NR_exit */
    asm volatile ("ecall" : : "r"(a0), "r"(a7) : "memory");
    while (1);
}
#endif

/* Include JIT subsystem */
#include "../src/jit.h"

/* Simplified CPUI386 context matching memory layout of i386.h */
typedef struct CPUI386 {
    union {
        uint32_t r32;
        uint16_t r16;
        uint8_t  r8[2];
    } gprx[8];
    uint32_t ip;
    uint32_t next_ip;
    uint32_t flags;
    uint32_t flags_mask;
    int cpl;
    bool code16;
    uint32_t sp_mask;
    bool halt;
    void *fpu;
    uint8_t guest_code[1024];
} CPUI386;

static CPUI386 test_cpu;

bool cpu_load8(CPUI386 *cpu, int seg, uint32_t addr, uint8_t *res) {
    (void)seg;
    if (addr < sizeof(cpu->guest_code)) {
        *res = cpu->guest_code[addr];
        return true;
    }
    return false;
}

int main(void) {
    raw_write_str("============================================================\n");
    raw_write_str("       Tiny386 - Phase 3 JIT Flow & Loop Verification       \n");
    raw_write_str("============================================================\n");

    jit_init();

    /* 
     * Synthesize 32-bit x86 machine code: Sum 100 to 1
     * EAX = 0 (sum)
     * ECX = 100 (counter)
     * EDX = 1 (step)
     * 
     * Offset 0x00:
     *   b8 00 00 00 00      mov eax, 0
     *   b9 64 00 00 00      mov ecx, 100
     *   ba 01 00 00 00      mov edx, 1
     * 
     * Offset 0x0F (Loop Header):
     *   01 c8               add eax, ecx
     *   29 d1               sub ecx, edx
     *   83 f9 00            cmp ecx, 0
     *   75 f7               jnz 0x0F (-9 bytes)
     * 
     * Offset 0x18:
     *   90                  nop (exit point)
     */
    uint8_t code[] = {
        0xb8, 0x00, 0x00, 0x00, 0x00, /* 0x00: mov eax, 0   */
        0xb9, 0x64, 0x00, 0x00, 0x00, /* 0x05: mov ecx, 100 */
        0xba, 0x01, 0x00, 0x00, 0x00, /* 0x0A: mov edx, 1   */
        0x01, 0xc8,                   /* 0x0F: add eax, ecx */
        0x29, 0xd1,                   /* 0x11: sub ecx, edx */
        0x83, 0xf9, 0x00,             /* 0x13: cmp ecx, 0   */
        0x75, 0xf7,                   /* 0x16: jnz 0x0F     */
        0x90                          /* 0x18: nop          */
    };

    memcpy(test_cpu.guest_code, code, sizeof(code));
    test_cpu.ip = 0;
    test_cpu.next_ip = 0;
    test_cpu.code16 = false;

    raw_write_str("[1] Initial state:\n");
    raw_write_str("    Guest code size: "); raw_write_dec(sizeof(code)); raw_write_str(" bytes\n");
    raw_write_str("    Initial EIP: 0x00\n");

    /*
     * Run dispatcher simulation:
     * 1. Check if compiled
     * 2. If not, check hotspot. If hotspot >= threshold -> compile
     * 3. Fallback: simple interpreter step for first 3 instructions
     */
    int steps = 0;
    int jit_executions = 0;
    int compiles = 0;

    while (test_cpu.ip < 0x18 && steps < 1000) {
        steps++;
        uint32_t pc = test_cpu.ip;

        void *entry = jit_lookup(pc);
        if (entry) {
            jit_executions++;
            ((jit_block_fn)entry)(&test_cpu);
            continue;
        }

        if (jit_should_compile(pc)) {
            raw_write_str("[2] Hotspot detected at EIP 0x");
            raw_write_hex(pc);
            raw_write_str("! Compiling Basic Block...\n");
            entry = jit_compile_block(&test_cpu, pc);
            if (entry) {
                compiles++;
                raw_write_str("    Compiled entry @ ");
                raw_write_hex((uint32_t)(uintptr_t)entry);
                raw_write_str("\n");
                ((jit_block_fn)entry)(&test_cpu);
                continue;
            }
        }

        /* Fallback: manually interpret the setup instructions */
        uint8_t op = test_cpu.guest_code[test_cpu.ip];
        if (op >= 0xB8 && op <= 0xBF) {
            int reg = op - 0xB8;
            uint32_t imm = *(uint32_t *)&test_cpu.guest_code[test_cpu.ip + 1];
            test_cpu.gprx[reg].r32 = imm;
            test_cpu.ip += 5;
            test_cpu.next_ip = test_cpu.ip;
        } else if (op == 0x01 && test_cpu.guest_code[test_cpu.ip + 1] == 0xC8) {
            test_cpu.gprx[0].r32 += test_cpu.gprx[1].r32;
            test_cpu.ip += 2;
            test_cpu.next_ip = test_cpu.ip;
        } else if (op == 0x29 && test_cpu.guest_code[test_cpu.ip + 1] == 0xD1) {
            test_cpu.gprx[1].r32 -= test_cpu.gprx[2].r32;
            test_cpu.ip += 2;
            test_cpu.next_ip = test_cpu.ip;
        } else if (op == 0x83) {
            test_cpu.ip += 3;
            test_cpu.next_ip = test_cpu.ip;
        } else if (op == 0x75) {
            if (test_cpu.gprx[1].r32 != 0) {
                test_cpu.ip = 0x0F;
            } else {
                test_cpu.ip += 2;
            }
            test_cpu.next_ip = test_cpu.ip;
        } else {
            test_cpu.ip++;
            test_cpu.next_ip = test_cpu.ip;
        }
    }

    raw_write_str("[3] Execution completed in ");
    raw_write_dec((uint32_t)steps);
    raw_write_str(" total steps\n");
    raw_write_str("    JIT Compilations: "); raw_write_dec((uint32_t)compiles); raw_write_str("\n");
    raw_write_str("    JIT Direct Executions: "); raw_write_dec((uint32_t)jit_executions); raw_write_str("\n");
    raw_write_str("    Final EAX (Sum): "); raw_write_dec(test_cpu.gprx[0].r32);
    raw_write_str(" (Expected: 5050)\n");
    raw_write_str("    Final ECX (Count): "); raw_write_dec(test_cpu.gprx[1].r32);
    raw_write_str(" (Expected: 0)\n");
    raw_write_str("    Final EIP: 0x"); raw_write_hex(test_cpu.ip);
    raw_write_str(" (Expected: 0x00000018)\n");

    if (test_cpu.gprx[0].r32 == 5050 && test_cpu.gprx[1].r32 == 0 && test_cpu.ip == 0x18 && compiles > 0) {
        raw_write_str("============================================================\n");
        raw_write_str("  >>> SUCCESS: RV32 JIT Loop & Fallback Verified! <<<\n");
        raw_write_str("============================================================\n");
        return 0;
    } else {
        raw_write_str(">>> ERROR: JIT Loop execution mismatch!\n");
        return 1;
    }
}
