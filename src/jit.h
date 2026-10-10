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

#if defined(__riscv)

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
 * @brief Lookup compiled RV32 block for given guest EIP
 */
void *jit_lookup(uint32_t guest_pc);

/**
 * @brief Lookup compiled RV32 block and return its instruction count in one scan.
 *        Returns NULL when no compiled block exists for guest_pc.
 */
void *jit_lookup_ex(uint32_t guest_pc, uint32_t *num_insns);

/**
 * @brief Get the instruction count in the basic block for given guest EIP
 */
uint32_t jit_get_block_insn_count(uint32_t guest_pc);

/**
 * @brief Check and update hotspot counter for given guest EIP
 */
bool jit_should_compile(uint32_t guest_pc);

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
 * @brief Get cumulative JIT block execution count
 */
uint32_t jit_get_block_exec_count(void);

/**
 * @brief Get cumulative JIT compilation count
 */
uint32_t jit_get_compile_count(void);

#else

/* Host dummy fallback for non-RISCV targets (e.g. PC native test) */
static inline void jit_init(void) {}
static inline void jit_enable(bool enable) { (void)enable; }
static inline bool jit_is_enabled(void) { return false; }
static inline void *jit_lookup(uint32_t guest_pc) { (void)guest_pc; return NULL; }
static inline uint32_t jit_get_block_insn_count(uint32_t guest_pc) { (void)guest_pc; return 1; }
static inline bool jit_should_compile(uint32_t guest_pc) { (void)guest_pc; return false; }
static inline void *jit_compile_block(CPUI386 *cpu, uint32_t guest_pc) { (void)cpu; (void)guest_pc; return NULL; }
static inline void jit_flush_cache(void) {}
static inline void jit_flush(void) {}
static inline uint32_t jit_get_block_exec_count(void) { return 0; }
static inline uint32_t jit_get_compile_count(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* JIT_H */
