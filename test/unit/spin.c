#include "test/jemalloc_test.h"

#include "jemalloc/internal/spin.h"

TEST_BEGIN(test_spin) {
	spin_t spinner = SPIN_INITIALIZER;

	for (unsigned i = 0; i < 100; i++) {
		spin_adaptive(&spinner);
	}
}
TEST_END

TEST_BEGIN(test_spin_cpu_spinwait) {
	/* Make sure that spin_boot() has run. */
	free(malloc(1));

	for (unsigned i = 0; i < 100; i++) {
		spin_cpu_spinwait();
	}
#ifdef JEMALLOC_HAVE_HWCAP_SB
	expect_b_eq(spin_use_sb, (getauxval(AT_HWCAP) & HWCAP_SB) != 0,
	    "spin_use_sb should match HWCAP_SB");
	/* Also run the CPU_SPINWAIT path on CPUs that implement sb. */
	bool use_sb = spin_use_sb;
	spin_use_sb = false;
	for (unsigned i = 0; i < 100; i++) {
		spin_cpu_spinwait();
	}
	spin_use_sb = use_sb;
#endif
}
TEST_END

int
main(void) {
	return test(test_spin, test_spin_cpu_spinwait);
}
