#ifndef JEMALLOC_INTERNAL_SPIN_H
#define JEMALLOC_INTERNAL_SPIN_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os.h"

#ifdef JEMALLOC_HAVE_HWCAP_SB
#	include <sys/auxv.h>

/* Whether the CPU implements sb; set by spin_boot(). */
extern bool spin_use_sb;
#endif

#define SPIN_INITIALIZER                                                       \
	{ 0U }

typedef struct {
	unsigned iteration;
} spin_t;

static inline void
spin_boot(void) {
#ifdef JEMALLOC_HAVE_HWCAP_SB
	spin_use_sb = (getauxval(AT_HWCAP) & HWCAP_SB) != 0;
#endif
}

static inline void
spin_cpu_spinwait(void) {
#ifdef JEMALLOC_HAVE_HWCAP_SB
	/*
	 * sb (speculation barrier, FEAT_SB) does not flush the pipeline the
	 * way isb does, but it also delays less.  On the CPU this was measured
	 * on, four back-to-back sb take about as long as one isb, so issue four
	 * to keep the duration of spin loops roughly unchanged.  Encoded with
	 * .inst so that older assemblers accept it.
	 */
	if (spin_use_sb) {
		/* sb; sb; sb; sb */
		__asm__ volatile(".inst 0xd50330ff\n\t.inst 0xd50330ff\n\t"
		    ".inst 0xd50330ff\n\t.inst 0xd50330ff");
		return;
	}
#endif
#if HAVE_CPU_SPINWAIT
	CPU_SPINWAIT;
#else
	volatile int x = 0;
	x = x;
#endif
}

static inline void
spin_adaptive(spin_t *spin) {
	volatile uint32_t i;

	if (spin->iteration < 5) {
		for (i = 0; i < (1U << spin->iteration); i++) {
			spin_cpu_spinwait();
		}
		spin->iteration++;
	} else {
		os_cpu_yield();
	}
}

#undef SPIN_INLINE

#endif /* JEMALLOC_INTERNAL_SPIN_H */
