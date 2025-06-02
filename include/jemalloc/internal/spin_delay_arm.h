#include "jemalloc/internal/jemalloc_preamble.h"

/* Global variable to track SB support */
extern int arm_has_sb_instruction;

/* Use SB instruction if available, otherwise ISB */
static inline void spin_delay_arm(void) {
	if (__builtin_expect(arm_has_sb_instruction == 1, 1)) {
		asm volatile(".inst 0xd50330ff \n");   /* SB instruction encoding */
	} else {
		asm volatile("isb; \n");
	}
}
