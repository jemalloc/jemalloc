#include "jemalloc/internal/jemalloc_preamble.h"
#include <stdatomic.h>

/* Global variable to track SB support, declared as extern to be defined in one TU */
extern _Atomic int arm_has_sb_instruction;

/* Constructor function declaration - implementation in spin_delay_arm.c */
__attribute__((constructor))
void detect_arm_sb_support(void);

/* Use SB instruction if available, otherwise ISB */
static inline void
spin_delay_arm(void) {
	if (__builtin_expect(arm_has_sb_instruction == 1, 1)) {
		/* SB instruction encoding */
		asm volatile(".inst 0xd50330ff \n");
	} else {
		/* ISB instruction */
		asm volatile("isb; \n");
	}
}
