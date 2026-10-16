#ifndef JIT_H
#define JIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUI386 CPUI386;

typedef void (*jit_block_fn)(CPUI386 *cpu);

#if !defined(TINY386_NO_JIT)
#if defined(__riscv) || defined(__aarch64__) || defined(_M_ARM64)
#define TINY386_HAS_JIT 1
#endif
#endif

#if defined(TINY386_HAS_JIT)

/**
 * @brief Initialize JIT subsystem and code buffers
 */
void jit_init(void);

/**
 * @brief Enable or disable JIT subsystem at runtime
 */
void jit_enable(bool enable);
bool jit_is_enabled(void);

/**
 * @brief Lookup compiled block for given guest EIP, validated against the
 *        physical page currently backing that virtual code page
 */
void *jit_lookup(CPUI386 *cpu, uint32_t guest_pc);

/**
 * @brief Lookup compiled block and return its instruction count in one scan.
 *        Returns NULL when no compiled block exists for guest_pc.
 */
void *jit_lookup_ex(CPUI386 *cpu, uint32_t guest_pc, uint32_t *num_insns);

/**
 * @brief Get the instruction count in the basic block for given guest EIP,
 *        validated against the physical page backing that virtual code page
 */
uint32_t jit_get_block_insn_count(CPUI386 *cpu, uint32_t guest_pc);

/**
 * @brief Check and update hotspot counter for given guest EIP
 */
bool jit_should_compile(CPUI386 *cpu, uint32_t guest_pc);

/**
 * @brief Dynamically compile a 32-bit x86 Basic Block starting at guest_pc
 */
void *jit_compile_block(CPUI386 *cpu, uint32_t guest_pc);

/**
 * @brief Flush and synchronize instruction cache
 */
void jit_flush_cache(void);

/**
 * @brief Invalidate all compiled JIT blocks and reset allocator
 */
void jit_flush(void);

/**
 * @brief Notify the JIT that guest virtual->physical mappings may have changed
 *        (TLB cleared / CR3 switched / CR0.PG toggled / INVLPG). Bumps the
 *        transition-cache epoch so cached block transitions are revalidated.
 */
void jit_note_map_change(void);

/**
 * @brief Get cumulative JIT block execution count
 */
uint32_t jit_get_block_exec_count(void);

/**
 * @brief Get cumulative JIT compilation count
 */
uint32_t jit_get_compile_count(void);

/* SMC (self-modifying code) tracking.
 * One byte per 4KB physical page: 1 = that page contains compiled JIT code.
 * Covers up to JIT_SMC_MAP_PAGES * 4096 bytes of guest RAM. */
#define JIT_SMC_MAP_PAGES 16384u
extern uint8_t *jit_smc_page_map;

/* Per-page generation counter, one byte per 4KB physical page. It is bumped
 * whenever a guest store lands on a page that currently holds compiled JIT
 * code. Every compiled block records the generation of the page it started in,
 * so a lookup can reject a stale block in O(1) instead of flushing the whole
 * hash table. jit_smc_dirty is now only set on 8-bit generation wrap-around,
 * which triggers a rare full reset. */
extern uint8_t *jit_page_gen;
extern volatile uint8_t jit_smc_dirty;

/* Absolute cpu->cycle value the current JIT dispatch session must stop at.
 * Set by the dispatcher before each block entry; chained blocks add their
 * retired instruction count directly to cpu->cycle and bail out of the chain
 * once cpu->cycle reaches this deadline, which is how the dispatcher recovers
 * the consumed step count without any C-side per-block accounting. */
extern long jit_cycle_deadline;

/* Inline flat-TLB fast path. A 512-entry direct-mapped cache of
 * linear-page -> host data-page mappings that the JIT probes inline before
 * falling back to cpu_load32()/cpu_store32(). An entry is only ever installed
 * for a page that is flat (segment base == 0), wholly inside guest RAM and not
 * MMIO, so a hit is equivalent to the C slow path. `host_delta` is the whole
 * trick: host_ptr = linear_addr + host_delta, one add away from the data.
 *
 * Entries are filled from the C slow path and invalidated (tag set to
 * JIT_FAST_TLB_INVALID) whenever the mapping may have changed -- tlb_clear()
 * (CR0.PG/CR0.WP/CR3/INVLPG) -- or the privilege level changes, since the page
 * permissions baked into an entry depend on cpl>0. */
#define JIT_FAST_TLB_BITS    9
#define JIT_FAST_TLB_SIZE    (1u << JIT_FAST_TLB_BITS)
#define JIT_FAST_TLB_MASK    (JIT_FAST_TLB_SIZE - 1u)
#define JIT_FAST_TLB_INVALID 0xFFFFFFFFu

#if defined(__aarch64__) || defined(_M_ARM64)
typedef struct {
	uint32_t vpage_tag;   /* linear page number, JIT_FAST_TLB_INVALID = free */
	uint32_t pad;
	uint64_t host_delta;  /* (uintptr_t)host_page_base - (vpage_tag << 12) */
} JitFastTLBEntry;        /* 16 bytes: host pointers are 64-bit */
#else
typedef struct {
	uint32_t vpage_tag;
	uint32_t host_delta;
} JitFastTLBEntry;        /* 8 bytes */
#endif

extern JitFastTLBEntry *jit_fast_tlb;
extern bool jit_fast_tlb_active;

/**
 * @brief Invalidate every fast-TLB entry (mapping or privilege level changed)
 */
void jit_fast_tlb_flush(void);

/**
 * @brief Mark the physical pages spanned by [start_va, end_va) as containing JIT code
 */
void jit_mark_code_pages(CPUI386 *cpu, uint32_t start_va, uint32_t end_va);

#else

/* Host dummy fallback for non-JIT targets */
static inline void jit_init(void) {}
static inline void jit_enable(bool enable) { (void)enable; }
static inline bool jit_is_enabled(void) { return false; }
static inline void *jit_lookup(CPUI386 *cpu, uint32_t guest_pc) { (void)cpu; (void)guest_pc; return NULL; }
static inline void *jit_lookup_ex(CPUI386 *cpu, uint32_t guest_pc, uint32_t *num_insns) { (void)cpu; if (num_insns) *num_insns = 1; (void)guest_pc; return NULL; }
static inline uint32_t jit_get_block_insn_count(CPUI386 *cpu, uint32_t guest_pc) { (void)cpu; (void)guest_pc; return 1; }
static inline bool jit_should_compile(CPUI386 *cpu, uint32_t guest_pc) { (void)cpu; (void)guest_pc; return false; }
static inline void *jit_compile_block(CPUI386 *cpu, uint32_t guest_pc) { (void)cpu; (void)guest_pc; return NULL; }
static inline void jit_flush_cache(void) {}
static inline void jit_flush(void) {}
static inline void jit_note_map_change(void) {}
static inline void jit_fast_tlb_flush(void) {}
static inline uint32_t jit_get_block_exec_count(void) { return 0; }
static inline uint32_t jit_get_compile_count(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* JIT_H */
