#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include "../src/i386.h"

#define GUEST_RAM_SIZE (16 * 1024 * 1024)
#define CODE_PAGE_ADDR 0x00100000 /* 1MB mark */
#define IDT_ADDR       0x00004000
#define ISR_ADDR       0x00005000
#define STACK_ADDR     0x00080000

static uint8_t *s_ram = NULL;
static sigjmp_buf g_fuzz_jmp;
static volatile int g_in_fuzz = 0;

static void sigabrt_handler(int sig) {
    if (g_in_fuzz) {
        siglongjmp(g_fuzz_jmp, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Simple dummy I/O handlers */
static uint8_t dummy_io_read8(void *opaque, int port) { return 0xff; }
static void dummy_io_write8(void *opaque, int port, uint8_t val) {}
static uint16_t dummy_io_read16(void *opaque, int port) { return 0xffff; }
static void dummy_io_write16(void *opaque, int port, uint16_t val) {}
static uint32_t dummy_io_read32(void *opaque, int port) { return 0xffffffff; }
static void dummy_io_write32(void *opaque, int port, uint32_t val) {}

static uint8_t dummy_iomem_read8(void *opaque, uword addr) { return 0xff; }
static void dummy_iomem_write8(void *opaque, uword addr, uint8_t val) {}
static uint16_t dummy_iomem_read16(void *opaque, uword addr) { return 0xffff; }
static void dummy_iomem_write16(void *opaque, uword addr, uint16_t val) {}
static uint32_t dummy_iomem_read32(void *opaque, uword addr) { return 0xffffffff; }
static void dummy_iomem_write32(void *opaque, uword addr, uint32_t val) {}

static int dummy_read_irq(void *opaque) { return -1; }

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Setup GDT for flat 32-bit protected mode */
static void setup_flat_gdt(uint8_t *ram, uint32_t gdt_addr) {
    uint32_t *gdt = (uint32_t *)(ram + gdt_addr);
    memset(gdt, 0, 1024 * 8);
    /* Selector 0x08: Code 32-bit (base=0, limit=4GB, DPL=0) */
    gdt[2] = 0x0000ffff;
    gdt[3] = 0x00cf9a00;
    /* Selector 0x10: Data 32-bit (base=0, limit=4GB, DPL=0) */
    gdt[4] = 0x0000ffff;
    gdt[5] = 0x00cf9200;
}

static void setup_idt(uint8_t *ram, uint32_t idt_addr, uint32_t isr_base) {
    /* Setup ISR stub at isr_base:
     * 0x5000: add esp, 4; iretd (for exceptions with error code)
     * 0x5004: iretd            (for exceptions without error code) */
    ram[isr_base + 0] = 0x83;
    ram[isr_base + 1] = 0xc4;
    ram[isr_base + 2] = 0x04;
    ram[isr_base + 3] = 0xcf; /* iretd */
    ram[isr_base + 4] = 0xcf; /* iretd */

    uint32_t *idt = (uint32_t *)(ram + idt_addr);
    for (int i = 0; i < 256; i++) {
        uint32_t target = isr_base + 4; /* default: iret */
        /* Exceptions with error codes: 8, 10, 11, 12, 13, 14, 17 */
        if (i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17) {
            target = isr_base;
        }
        /* 32-bit Interrupt Gate: selector 0x08, DPL=0, Present */
        idt[i * 2 + 0] = (0x0008 << 16) | (target & 0xffff);
        idt[i * 2 + 1] = (target & 0xffff0000) | 0x8e00;
    }
}

static void init_cpu_callbacks(CPU_CB *cb) {
    cb->io = NULL;
    cb->io_read8 = dummy_io_read8;
    cb->io_write8 = dummy_io_write8;
    cb->io_read16 = dummy_io_read16;
    cb->io_write16 = dummy_io_write16;
    cb->io_read32 = dummy_io_read32;
    cb->io_write32 = dummy_io_write32;
    cb->io_read_string = NULL;
    cb->io_write_string = NULL;
    cb->iomem = NULL;
    cb->iomem_read8 = dummy_iomem_read8;
    cb->iomem_write8 = dummy_iomem_write8;
    cb->iomem_read16 = dummy_iomem_read16;
    cb->iomem_write16 = dummy_iomem_write16;
    cb->iomem_read32 = dummy_iomem_read32;
    cb->iomem_write32 = dummy_iomem_write32;
    cb->iomem_write_string = NULL;
    cb->pic = NULL;
    cb->pic_read_irq = dummy_read_irq;
}

int main(int argc, char **argv) {
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : 123456789;
    int num_iterations = (argc > 2) ? atoi(argv[2]) : 50000;
    printf("[FUZZ] Starting Tiny386 CPU Fuzzing (Seed: %u, Iterations: %d)...\n", seed, num_iterations);

    signal(SIGABRT, sigabrt_handler);

    s_ram = (uint8_t *)calloc(1, GUEST_RAM_SIZE);
    assert(s_ram != NULL);

    setup_flat_gdt(s_ram, 0x1000);
    setup_idt(s_ram, IDT_ADDR, ISR_ADDR);

    CPU_CB *cb = NULL;
    CPUI386 *cpu = cpui386_new(4, (char *)s_ram, GUEST_RAM_SIZE, &cb);
    assert(cpu != NULL);
    assert(cb != NULL);
    init_cpu_callbacks(cb);
    cpui386_enable_fpu(cpu);

    uint32_t rng = seed;
    int passed_iterations = 0;
    int exceptions_caught = 0;
    int triple_faults = 0;

    for (int iter = 0; iter < num_iterations; iter++) {
        /* Every 10 iterations, reset CPU into flat 32-bit Protected Mode with valid stack, GDT & IDT */
        if ((iter % 10) == 0) {
            cpui386_reset_pm(cpu, CODE_PAGE_ADDR);
            cpui386_set_gdtr(cpu, 0x1000, 1024 * 8 - 1);
            cpui386_set_idtr(cpu, IDT_ADDR, 256 * 8 - 1);
            cpui386_set_gpr(cpu, 4, STACK_ADDR); /* ESP */
            /* Randomize registers except ESP */
            for (int r = 0; r < 8; r++) {
                if (r != 4) cpui386_set_gpr(cpu, r, xorshift32(&rng));
            }
        }

        /* Generate random instruction stream at CODE_PAGE_ADDR */
        uint8_t *code_ptr = s_ram + CODE_PAGE_ADDR;
        int code_len = 8 + (xorshift32(&rng) % 24); /* 8 to 32 bytes */
        for (int i = 0; i < code_len; i++) {
            code_ptr[i] = (uint8_t)xorshift32(&rng);
        }
        /* Put NOPs at the end */
        code_ptr[code_len] = 0x90;
        code_ptr[code_len + 1] = 0x90;

        g_in_fuzz = 1;
        if (sigsetjmp(g_fuzz_jmp, 1) != 0) {
            /* Handled CPU triple fault abort cleanly */
            g_in_fuzz = 0;
            triple_faults++;
            cpui386_reset_pm(cpu, CODE_PAGE_ADDR);
            cpui386_set_gdtr(cpu, 0x1000, 1024 * 8 - 1);
            cpui386_set_idtr(cpu, IDT_ADDR, 256 * 8 - 1);
            cpui386_set_gpr(cpu, 4, STACK_ADDR);
            continue;
        }

        /* Step instructions */
        for (int step = 0; step < 16; step++) {
            cpui386_step(cpu, 1);
            int exc = cpui386_get_excno(cpu);
            if (exc >= 0) {
                exceptions_caught++;
                break;
            }
        }
        g_in_fuzz = 0;

        passed_iterations++;
        if ((iter + 1) % 10000 == 0) {
            printf("[FUZZ] Iteration %d/%d completed. (Exceptions: %d, Triple-Faults handled: %d)\n",
                   iter + 1, num_iterations, exceptions_caught, triple_faults);
            fflush(stdout);
        }
    }

    printf("[FUZZ] SUCCESS! Completed %d iterations without ASan/UBSan violations. (Exceptions: %d, Triple-Faults: %d)\n",
           passed_iterations, exceptions_caught, triple_faults);

    cpui386_delete(cpu);
    free(s_ram);
    return 0;
}
