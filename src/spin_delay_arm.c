#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/spin_delay_arm.h"
#include <stdatomic.h>

#if defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
#include <sys/auxv.h>

/* Define HWCAP_SB if not already defined in system headers */
#ifndef HWCAP_SB
#define HWCAP_SB (1ULL << 56) /* Speculation Barrier */
#endif   // HWCAP_SB
#endif   // __linux__ && (defined(__aarch64__) || defined(__arm64__))

/* Global variable to track SB support, defined here to avoid multiple definitions */
_Atomic int arm_has_sb_instruction = ATOMIC_VAR_INIT(0);

/* Constructor function to detect hardware capabilities at program startup */
__attribute__((constructor))
void
detect_arm_sb_support(void) {
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
	/* Check if SB instruction is supported */
	if (getauxval(AT_HWCAP) & HWCAP_SB) {
		atomic_store_explicit(&arm_has_sb_instruction, 1, memory_order_release);
	}
#endif
}