#ifndef I386_STRUCT_H
#define I386_STRUCT_H

#include "i386.h"
#include <stddef.h>

#ifndef I386_OPT1
#define I386_OPT1
#endif

#ifdef I386_ENABLE_FPU
#include "fpu.h"
#else
#ifndef FPU_DEFINED
#define FPU_DEFINED
typedef void FPU;
#endif
#endif

struct CPUI386 {
#ifdef I386_OPT1
	union {
		u32 r32;
		u16 r16;
		u8 r8[2];
	} gprx[8];
#else
	uword gpr[8];
#endif
	uword ip, next_ip;
	uword flags;
	uword flags_mask;
	int cpl;
	bool code16;
	uword sp_mask;
	bool halt;

	FPU *fpu;

	struct {
		uword sel;
		uword base;
		uword limit;
		uword flags;
	} seg[8];

	struct {
		uword base;
		uword limit;
	} idt, gdt;

	uword cr0, cr2, cr3;

	uword dr[8];

	struct {
		unsigned long laddr;
		uword xaddr;
		uword paddr;
	} ifetch;

	struct {
		int op;
		uword dst;
		uword dst2;
		uword src1;
		uword src2;
		uword mask;
	} cc;

	struct {
		int size;
		struct tlb_entry {
			uword lpgno;
			uword xaddr;
			int (*pte_lookup)[2];
			u8 *ppte;
		} *tab;
	} tlb;

	u8 *phys_mem;
	long phys_mem_size;

	long cycle;

	int excno;
	uword excerr;

	bool intr;
	CPU_CB cb;

	int gen;
	struct {
		uword cs, eip, esp;
	} sysenter;

#ifdef AMD64_ENABLE_LEG32
	struct {
		u32 efer;
	} lm;
#define INIT_LM_EFER(cpu) ((cpu)->lm.efer = 0)
#define GET_LM_EFER(cpu) ((cpu)->lm.efer)
#define SET_LM_EFER(cpu, x) ((cpu)->lm.efer = (x))
#define CPUID_LM0_2 0x2048
#define CPUID_LM80_0 0x80000004
#define CPUID_LM81_2 ((1<<29) | (1<<11))
#else
#define INIT_LM_EFER(cpu)
#define GET_LM_EFER(cpu) 0
#define SET_LM_EFER(cpu, x) THROW(EX_GP, 0)
#define CPUID_LM0_2 0
#define CPUID_LM80_0 0
#define CPUID_LM81_2 0
#endif
};

#if defined(__riscv) || defined(BUILD_ESP32)
_Static_assert(offsetof(struct CPUI386, intr) == 320, "intr offset rv32");
#endif
#if defined(__aarch64__)
_Static_assert(offsetof(struct CPUI386, intr) == 352, "intr offset arm64");
_Static_assert(offsetof(struct CPUI386, cc.mask) == 300, "cc.mask offset arm64");
_Static_assert(offsetof(struct CPUI386, flags) == 40, "flags offset arm64");
#endif

#endif /* I386_STRUCT_H */
