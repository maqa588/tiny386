/*
 * Tiny386 JIT throughput benchmark (host-side, deterministic).
 *
 * Runs a fixed-count, register-only hot loop on a flat 32-bit protected-mode
 * guest with real RAM, once with the JIT enabled and once with it disabled
 * (pure interpreter), and reports cycles/second plus the JIT/interpreter ratio.
 * This is a CPU-bound proxy for an in-guest benchmark such as CPU-Z, and it
 * exercises exactly the code path the JIT accelerates (PM user code).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "../src/i386.h"
#include "../src/jit.h"

#define GUEST_RAM_SIZE (16 * 1024 * 1024)
#define CODE_ADDR      0x00100000
#define IDT_ADDR       0x00004000
#define ISR_ADDR       0x00004080
#define STACK_ADDR     0x00080000
#define GDT_ADDR       0x00001000

static uint8_t dummy_iomem_rd8(void *o, uword a) { (void)o; (void)a; return 0xff; }
static void dummy_iomem_wr8(void *o, uword a, uint8_t v) { (void)o; (void)a; (void)v; }
static int dummy_read_irq(void *o) { (void)o; return -1; }

static void init_cpu_callbacks(CPU_CB *cb) {
    memset(cb, 0, sizeof(*cb));
    cb->iomem_read8 = dummy_iomem_rd8;
    cb->iomem_write8 = dummy_iomem_wr8;
    cb->pic_read_irq = dummy_read_irq;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void setup_guest(uint8_t *ram) {
    uint32_t *gdt = (uint32_t *)(ram + GDT_ADDR);
    memset(gdt, 0, 1024);
    gdt[2] = 0x0000ffff; gdt[3] = 0x00cf9a00; /* code 32-bit, base0, limit4G */
    gdt[4] = 0x0000ffff; gdt[5] = 0x00cf9200; /* data 32-bit */

    uint32_t *idt = (uint32_t *)(ram + IDT_ADDR);
    for (int i = 0; i < 256; i++) {
        uint32_t target = ISR_ADDR + 4;
        if (i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17)
            target = ISR_ADDR;
        idt[i * 2 + 0] = (0x0008u << 16) | (target & 0xffff);
        idt[i * 2 + 1] = (target & 0xffff0000u) | 0x8e00u;
    }

    /* Register-only hot loop, using only ops both the JIT and interpreter handle:
     *   mov eax,0 ; mov ecx,100 ; mov edx,1
     * loop: add eax,ecx ; sub ecx,edx ; cmp ecx,0 ; jnz loop ; jmp loop
     */
    static const uint8_t code[] = {
        0xb8, 0x00, 0x00, 0x00, 0x00,   /* 0x00 mov eax,0 */
        0xb9, 0x64, 0x00, 0x00, 0x00,   /* 0x05 mov ecx,100 */
        0xba, 0x01, 0x00, 0x00, 0x00,   /* 0x0A mov edx,1 */
        0x01, 0xc8,                     /* 0x0F add eax,ecx */
        0x29, 0xd1,                     /* 0x11 sub ecx,edx */
        0x83, 0xf9, 0x00,               /* 0x13 cmp ecx,0 */
        0x75, 0xf7,                     /* 0x16 jnz 0x0F */
        0xeb, 0xf5,                     /* 0x18 jmp 0x0F */
    };
    memcpy(ram + CODE_ADDR, code, sizeof(code));
}

static void enter_guest(CPUI386 *cpu) {
    cpui386_reset_pm(cpu, CODE_ADDR);
    cpui386_set_gdtr(cpu, GDT_ADDR, 1024 * 8 - 1);
    cpui386_set_idtr(cpu, IDT_ADDR, 256 * 8 - 1);
    cpui386_set_gpr(cpu, 4, STACK_ADDR); /* ESP */
}

static double run_timed(CPUI386 *cpu, uint32_t warm, uint32_t steps, long *cycles_out) {
    enter_guest(cpu);
    if (warm) cpui386_step(cpu, (int)warm);
    cpui386_reset_pm(cpu, CODE_ADDR);
    cpui386_set_gdtr(cpu, GDT_ADDR, 1024 * 8 - 1);
    cpui386_set_idtr(cpu, IDT_ADDR, 256 * 8 - 1);
    cpui386_set_gpr(cpu, 4, STACK_ADDR);
    long c0 = cpui386_get_cycle(cpu);
    double t0 = now_sec();
    cpui386_step(cpu, (int)steps);
    double t1 = now_sec();
    long c1 = cpui386_get_cycle(cpu);
    if (cycles_out) *cycles_out = c1 - c0;
    return t1 - t0;
}

int main(int argc, char **argv) {
    uint32_t steps = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 200000000u;
    uint32_t warm  = 20000000u;

    uint8_t *ram = (uint8_t *)calloc(1, GUEST_RAM_SIZE);
    assert(ram != NULL);
    setup_guest(ram);

    CPU_CB *cb = NULL;
    CPUI386 *cpu = cpui386_new(4, (char *)ram, GUEST_RAM_SIZE, &cb);
    assert(cpu != NULL && cb != NULL);
    init_cpu_callbacks(cb);
    cpui386_enable_fpu(cpu);

    printf("=== Tiny386 JIT throughput benchmark ===\n");
    printf("timed steps per run: %u\n", steps);

    /* JIT ON */
    jit_init();
    jit_enable(true);
    jit_flush();
    long cyc_jit = 0;
    double tj = run_timed(cpu, warm, steps, &cyc_jit);
    uint32_t comp = jit_get_compile_count();
    uint32_t exec = jit_get_block_exec_count();

    /* JIT OFF (pure interpreter) */
    jit_enable(false);
    jit_flush();
    long cyc_int = 0;
    double ti = run_timed(cpu, 0, steps, &cyc_int);

    double mips_jit = (double)cyc_jit / tj / 1e6;
    double mips_int = (double)cyc_int / ti / 1e6;

    printf("JIT ON : %ld cycles in %.3fs -> %.1f MIPS  (compiles=%u, block-execs=%u)\n",
           cyc_jit, tj, mips_jit, (unsigned)comp, (unsigned)exec);
    printf("JIT OFF: %ld cycles in %.3fs -> %.1f MIPS  (interpreter)\n",
           cyc_int, ti, mips_int);
    printf("speedup: %.2fx  (JIT/interpreted)\n", mips_jit / mips_int);

    cpui386_delete(cpu);
    free(ram);
    return 0;
}
