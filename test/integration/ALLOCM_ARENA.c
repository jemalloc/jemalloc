#include "test/jemalloc_test.h"

#define	NTHREADS 10

void *
je_thread_start(void *arg)
{
	unsigned thread_ind = (unsigned)(uintptr_t)arg;
	unsigned arena_ind;
	void *p;
	size_t rsz, sz;

	sz = sizeof(arena_ind);
	assert_d_eq(mallctl("arenas.extend", &arena_ind, &sz, NULL, 0), 0,
	    "Error in arenas.extend");

	if (thread_ind % 4 != 3) {
		size_t mib[3];
		size_t miblen = sizeof(mib) / sizeof(size_t);
		const char *dss_precs[] = {"disabled", "primary", "secondary"};
		const char *dss = dss_precs[thread_ind %
		    (sizeof(dss_precs)/sizeof(char*))];
		assert_d_eq(mallctlnametomib("arena.0.dss", mib, &miblen), 0,
		    "Error in mallctlnametomib()");
		mib[1] = arena_ind;
		assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&dss,
		    sizeof(const char *)), 0, "Error in mallctlbymib()");
	}

	assert_d_eq(allocm(&p, &rsz, 1, ALLOCM_ARENA(arena_ind)),
	    ALLOCM_SUCCESS, "Unexpected allocm() error");
	dallocm(p, 0);

	return (NULL);
}

TEST_BEGIN(test_ALLOCM_ARENA)
{
	je_thread_t threads[NTHREADS];
	unsigned i;

	for (i = 0; i < NTHREADS; i++) {
		je_thread_create(&threads[i], je_thread_start,
		    (void *)(uintptr_t)i);
	}

	for (i = 0; i < NTHREADS; i++)
		je_thread_join(threads[i], NULL);
}
TEST_END

int
main(void)
{

	return (test(
	    test_ALLOCM_ARENA));
}
