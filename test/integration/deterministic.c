#include "test/jemalloc_test.h"
#include "jemalloc/internal/deterministic.h"

#if defined(JEMALLOC_DETERMINISTIC_SCHED)
static unsigned	long	arena_ind;

static bool
disable_background_thread(void) {
	bool enabled = false;
	int ret = mallctl("background_thread", NULL, NULL,
			  (void *)&enabled, sizeof(bool));
	if (ret == ENOENT) {
		return false;
	}
	assert_d_eq(ret, 0, "Unexpected mallctl error");
	return enabled;
}

static unsigned
do_arena_create(extent_hooks_t *h) {
	unsigned arena_ind;
	size_t sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz,
	    (void *)(h != NULL ? &h : NULL), (h != NULL ? sizeof(h) : 0)), 0,
	    "Unexpected mallctl() failure");
	return arena_ind;
}

static void
do_arena_destroy(unsigned arena_ind) {
	size_t mib[3];
	size_t miblen;

	miblen = sizeof(mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.destroy", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)arena_ind;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static void*
mythread(void* arg) {
	for (int i = 0; i < 30; i++) {
		size_t sz = random() % 32000;
		size_t sz2 = random() % 32000;
		if (sz == 0) sz++;
		size_t align = 0;
		while (sz > (1UL << (align +1))) {
			align++;
		}
		if (sz2 == 0) sz2++;

		void *p = mallocx(sz, MALLOCX_ARENA(arena_ind) |
				      MALLOCX_TCACHE_NONE |
				      MALLOCX_LG_ALIGN(align)
				 );
		p = rallocx(p, sz2, MALLOCX_ARENA(arena_ind) |
				    MALLOCX_TCACHE_NONE
			   );
		dallocx(p, MALLOCX_ARENA(arena_ind) |
			   MALLOCX_TCACHE_NONE);
	}
	return NULL;
}
#endif /* JEMALLOC_DETERMINISTIC_SCHED */

/* A single run is probably not enough, suggested usage is:
 * for i in `seq 1 100`; do echo $i; SEED=$i MALLOC_CONF="tcache:false,narenas:1"  ./test/integration/deterministic ; done
 */
TEST_BEGIN(test_deterministic) {
#if defined(JEMALLOC_DETERMINISTIC_SCHED)
	/* Not yet integrated with background thread */
	disable_background_thread();

	int numthreads = 40;

	arena_ind = do_arena_create(NULL);
	char* seed = getenv("SEED");
	int s = 0;
	if (seed) {
		s = atoi(seed);
	}
	srandom(s);
	det_schedule(s, numthreads, &mythread, (void*)arena_ind);

	do_arena_destroy((unsigned int)arena_ind);

#else /* JEMALLOC_DETERMINISTIC_SCHED */
	test_skip("no deterministic schedule support");
#endif /* JEMALLOC_DETERMINISTIC_SCHED */
}
TEST_END

int
main(void) {
	return test(
		test_deterministic);
}
