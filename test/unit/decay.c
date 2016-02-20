#include "test/jemalloc_test.h"

const char *malloc_conf = "purge:decay,decay_time:1";

static time_update_t *time_update_orig;

static unsigned nupdates_mock;
static struct timespec time_mock;
static bool nonmonotonic_mock;

static bool
time_update_mock(struct timespec *time)
{

	nupdates_mock++;
	if (!nonmonotonic_mock)
		time_copy(time, &time_mock);
	return (nonmonotonic_mock);
}

TEST_BEGIN(test_decay_ticks)
{
	ticker_t *decay_ticker;
	unsigned tick0, tick1;
	size_t sz, huge0, large0;
	void *p;
	unsigned tcache_ind;

	test_skip_if(opt_purge != purge_mode_decay);

	decay_ticker = decay_ticker_get(tsd_fetch(), 0);
	assert_ptr_not_null(decay_ticker,
	    "Unexpected failure getting decay ticker");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.hchunk.0.size", &huge0, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	assert_d_eq(mallctl("arenas.lrun.0.size", &large0, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	/* malloc(). */
	tick0 = ticker_read(decay_ticker);
	p = malloc(huge0);
	assert_ptr_not_null(p, "Unexpected malloc() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0, "Expected ticker to tick during malloc()");
	/* free(). */
	tick0 = ticker_read(decay_ticker);
	free(p);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0, "Expected ticker to tick during free()");

	/* calloc(). */
	tick0 = ticker_read(decay_ticker);
	p = calloc(1, huge0);
	assert_ptr_not_null(p, "Unexpected calloc() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0, "Expected ticker to tick during calloc()");
	free(p);

	/* posix_memalign(). */
	tick0 = ticker_read(decay_ticker);
	assert_d_eq(posix_memalign(&p, sizeof(size_t), huge0), 0,
	    "Unexpected posix_memalign() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during posix_memalign()");
	free(p);

	/* aligned_alloc(). */
	tick0 = ticker_read(decay_ticker);
	p = aligned_alloc(sizeof(size_t), huge0);
	assert_ptr_not_null(p, "Unexpected aligned_alloc() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during aligned_alloc()");
	free(p);

	/* realloc(). */
	/* Allocate. */
	tick0 = ticker_read(decay_ticker);
	p = realloc(NULL, huge0);
	assert_ptr_not_null(p, "Unexpected realloc() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0, "Expected ticker to tick during realloc()");
	/* Reallocate. */
	tick0 = ticker_read(decay_ticker);
	p = realloc(p, huge0);
	assert_ptr_not_null(p, "Unexpected realloc() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0, "Expected ticker to tick during realloc()");
	/* Deallocate. */
	tick0 = ticker_read(decay_ticker);
	realloc(p, 0);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0, "Expected ticker to tick during realloc()");

	/* Huge mallocx(). */
	tick0 = ticker_read(decay_ticker);
	p = mallocx(huge0, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during huge mallocx()");
	/* Huge rallocx(). */
	tick0 = ticker_read(decay_ticker);
	p = rallocx(p, huge0, 0);
	assert_ptr_not_null(p, "Unexpected rallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during huge rallocx()");
	/* Huge xallocx(). */
	tick0 = ticker_read(decay_ticker);
	xallocx(p, huge0, 0, 0);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during huge xallocx()");
	/* Huge dallocx(). */
	tick0 = ticker_read(decay_ticker);
	dallocx(p, 0);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during huge dallocx()");
	/* Huge sdallocx(). */
	p = mallocx(huge0, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick0 = ticker_read(decay_ticker);
	sdallocx(p, huge0, 0);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during huge sdallocx()");

	/* Large mallocx(). */
	tick0 = ticker_read(decay_ticker);
	p = mallocx(large0, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during large mallocx()");
	/* Large rallocx(). */
	tick0 = ticker_read(decay_ticker);
	p = rallocx(p, large0, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected rallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during large rallocx()");
	/* Large xallocx(). */
	tick0 = ticker_read(decay_ticker);
	xallocx(p, large0, 0, MALLOCX_TCACHE_NONE);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during large xallocx()");
	/* Large dallocx(). */
	tick0 = ticker_read(decay_ticker);
	dallocx(p, MALLOCX_TCACHE_NONE);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during large dallocx()");
	/* Large sdallocx(). */
	p = mallocx(large0, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick0 = ticker_read(decay_ticker);
	sdallocx(p, large0, MALLOCX_TCACHE_NONE);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during large sdallocx()");

	/* Small mallocx(). */
	tick0 = ticker_read(decay_ticker);
	p = mallocx(1, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during small mallocx()");
	/* Small rallocx(). */
	tick0 = ticker_read(decay_ticker);
	p = rallocx(p, 1, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected rallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during small rallocx()");
	/* Small xallocx(). */
	tick0 = ticker_read(decay_ticker);
	xallocx(p, 1, 0, MALLOCX_TCACHE_NONE);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during small xallocx()");
	/* Small dallocx(). */
	tick0 = ticker_read(decay_ticker);
	dallocx(p, MALLOCX_TCACHE_NONE);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during small dallocx()");
	/* Small sdallocx(). */
	p = mallocx(1, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick0 = ticker_read(decay_ticker);
	sdallocx(p, 1, MALLOCX_TCACHE_NONE);
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during small sdallocx()");

	/* tcache fill. */
	sz = sizeof(unsigned);
	assert_d_eq(mallctl("tcache.create", &tcache_ind, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	tick0 = ticker_read(decay_ticker);
	p = mallocx(1, MALLOCX_TCACHE(tcache_ind));
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during tcache fill");
	/* tcache flush. */
	dallocx(p, MALLOCX_TCACHE(tcache_ind));
	tick0 = ticker_read(decay_ticker);
	assert_d_eq(mallctl("tcache.flush", NULL, NULL, &tcache_ind,
	    sizeof(unsigned)), 0, "Unexpected mallctl failure");
	tick1 = ticker_read(decay_ticker);
	assert_u32_ne(tick1, tick0,
	    "Expected ticker to tick during tcache flush");
}
TEST_END

TEST_BEGIN(test_decay_ticker)
{
#define	NPS 1024
	int flags = (MALLOCX_ARENA(0) | MALLOCX_TCACHE_NONE);
	void *ps[NPS];
	uint64_t epoch, npurge0, npurge1;
	size_t sz, tcache_max, large;
	unsigned i, nupdates0;
	struct timespec time, decay_time, deadline;

	test_skip_if(opt_purge != purge_mode_decay);

	/*
	 * Allocate a bunch of large objects, pause the clock, deallocate the
	 * objects, restore the clock, then [md]allocx() in a tight loop to
	 * verify the ticker triggers purging.
	 */

	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.tcache_max", &tcache_max, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	large = nallocx(tcache_max + 1, flags);

	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl failure");
	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(uint64_t)), 0,
	    "Unexpected mallctl failure");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.npurge", &npurge0, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	for (i = 0; i < NPS; i++) {
		ps[i] = mallocx(large, flags);
		assert_ptr_not_null(ps[i], "Unexpected mallocx() failure");
	}

	nupdates_mock = 0;
	time_init(&time_mock, 0, 0);
	time_update(&time_mock);
	nonmonotonic_mock = false;

	time_update_orig = time_update;
	time_update = time_update_mock;

	for (i = 0; i < NPS; i++) {
		dallocx(ps[i], flags);
		nupdates0 = nupdates_mock;
		assert_d_eq(mallctl("arena.0.decay", NULL, NULL, NULL, 0), 0,
		    "Unexpected arena.0.decay failure");
		assert_u_gt(nupdates_mock, nupdates0,
		    "Expected time_update() to be called");
	}

	time_update = time_update_orig;

	time_init(&time, 0, 0);
	time_update(&time);
	time_init(&decay_time, opt_decay_time, 0);
	time_copy(&deadline, &time);
	time_add(&deadline, &decay_time);
	do {
		for (i = 0; i < DECAY_NTICKS_PER_UPDATE / 2; i++) {
			void *p = mallocx(1, flags);
			assert_ptr_not_null(p, "Unexpected mallocx() failure");
			dallocx(p, flags);
		}
		assert_d_eq(mallctl("epoch", NULL, NULL, &epoch,
		    sizeof(uint64_t)), 0, "Unexpected mallctl failure");
		sz = sizeof(uint64_t);
		assert_d_eq(mallctl("stats.arenas.0.npurge", &npurge1, &sz,
		    NULL, 0), 0, "Unexpected mallctl failure");

		time_update(&time);
	} while (time_compare(&time, &deadline) <= 0 && npurge1 == npurge0);

	assert_u64_gt(npurge1, npurge0, "Expected purging to occur");
#undef NPS
}
TEST_END

TEST_BEGIN(test_decay_nonmonotonic)
{
#define	NPS (SMOOTHSTEP_NSTEPS + 1)
	int flags = (MALLOCX_ARENA(0) | MALLOCX_TCACHE_NONE);
	void *ps[NPS];
	uint64_t epoch, npurge0, npurge1;
	size_t sz, large0;
	unsigned i, nupdates0;

	test_skip_if(opt_purge != purge_mode_decay);

	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.lrun.0.size", &large0, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl failure");
	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(uint64_t)), 0,
	    "Unexpected mallctl failure");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.npurge", &npurge0, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	nupdates_mock = 0;
	time_init(&time_mock, 0, 0);
	time_update(&time_mock);
	nonmonotonic_mock = true;

	time_update_orig = time_update;
	time_update = time_update_mock;

	for (i = 0; i < NPS; i++) {
		ps[i] = mallocx(large0, flags);
		assert_ptr_not_null(ps[i], "Unexpected mallocx() failure");
	}

	for (i = 0; i < NPS; i++) {
		dallocx(ps[i], flags);
		nupdates0 = nupdates_mock;
		assert_d_eq(mallctl("arena.0.decay", NULL, NULL, NULL, 0), 0,
		    "Unexpected arena.0.decay failure");
		assert_u_gt(nupdates_mock, nupdates0,
		    "Expected time_update() to be called");
	}

	assert_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(uint64_t)), 0,
	    "Unexpected mallctl failure");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.npurge", &npurge1, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	assert_u64_gt(npurge1, npurge0, "Expected purging to occur");

	time_update = time_update_orig;
#undef NPS
}
TEST_END

int
main(void)
{

	return (test(
	    test_decay_ticks,
	    test_decay_ticker,
	    test_decay_nonmonotonic));
}
