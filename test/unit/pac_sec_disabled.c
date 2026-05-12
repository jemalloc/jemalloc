#include "test/jemalloc_test.h"

const char *malloc_conf = "pac_sec_nshards:0,background_thread:false";

static size_t
read_stat(unsigned arena_ind, const char *field) {
	char     cmd[128];
	size_t   val;
	uint64_t epoch = 1;
	size_t   sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sz), 0,
	    "Unexpected mallctl failure");
	sz = sizeof(val);
	snprintf(cmd, sizeof(cmd), "stats.arenas.%u.pac_sec_%s",
	    arena_ind, field);
	expect_d_eq(mallctl(cmd, (void *)&val, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure reading pac_sec stat");
	return val;
}

TEST_BEGIN(test_pac_sec_disabled) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	size_t nshards;
	size_t sz = sizeof(nshards);
	expect_d_eq(mallctl("opt.pac_sec_nshards", (void *)&nshards,
	    &sz, NULL, 0), 0, "opt.pac_sec_nshards read failed");
	expect_zu_eq(nshards, 0,
	    "test precondition: pac_sec_nshards must be 0");

	unsigned arena_ind;
	sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "arenas.create failed");

	int    flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = SC_LARGE_MINCLASS;

	/*
	 * With SEC disabled, allocs and dallocs go straight to the ecaches.
	 * No SEC counter should ever bump.
	 */
	void *p = mallocx(alloc_size, flags);
	expect_ptr_not_null(p, "mallocx failed");
	dallocx(p, flags);

	expect_zu_eq(read_stat(arena_ind, "hits"), 0,
	    "no hits when SEC is disabled");
	expect_zu_eq(read_stat(arena_ind, "misses"), 0,
	    "no misses when SEC is disabled");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "no bytes cached when SEC is disabled");
	expect_zu_eq(read_stat(arena_ind, "dalloc_noflush"), 0,
	    "no absorbs when SEC is disabled");
	expect_zu_eq(read_stat(arena_ind, "dalloc_flush"), 0,
	    "no flushes when SEC is disabled");

	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(mallctl(buf, NULL, NULL, NULL, 0), 0,
	    "arena destroy failed");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_pac_sec_disabled);
}
