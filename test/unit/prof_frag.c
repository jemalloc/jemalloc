#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"

/*
 * The tests below capture heap dumps (produced with lg_prof_sample:0, i.e.
 * every allocation sampled) via the JET-mutable prof_dump_{open,write}_file
 * hooks, and then parse the fragmentation records:
 *   f: <age_ns> <request_size> <usize> <szind> <arena_ind> <thr_uid>
 *   frag_util: <arena_ind> <binind> <reg_size> <slab_size> <nregs> <n_shards>
 *       <curslabs> <curregs> <nonfull_slabs>
 * Records of interest are identified by uncommon request sizes, so that
 * allocations made by the test harness itself do not interfere.
 */

static const char *test_filename = "test_filename";

#define DUMP_BUF_SIZE (16u << 20)
static char   dump_buf[DUMP_BUF_SIZE];
static size_t dump_len;
static bool   dump_discard;

static prof_dump_open_file_t  *open_file_orig;
static prof_dump_write_file_t *write_file_orig;

static int
prof_dump_open_file_intercept(const char *filename, int mode) {
	int fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");
	return fd;
}

static ssize_t
prof_dump_write_file_intercept(int fd, const void *s, size_t len) {
	if (!dump_discard) {
		assert_zu_le(dump_len + len, DUMP_BUF_SIZE,
		    "Dump capture buffer overflow");
		memcpy(&dump_buf[dump_len], s, len);
		dump_len += len;
	}
	return (ssize_t)len;
}

static void
intercepts_install(void) {
	open_file_orig = prof_dump_open_file;
	write_file_orig = prof_dump_write_file;
	prof_dump_open_file = prof_dump_open_file_intercept;
	prof_dump_write_file = prof_dump_write_file_intercept;
}

static void
intercepts_restore(void) {
	prof_dump_open_file = open_file_orig;
	prof_dump_write_file = write_file_orig;
}

static void
dump(void) {
	dump_len = 0;
	dump_buf[0] = '\0';
	assert_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&test_filename,
	                sizeof(test_filename)),
	    0, "Unexpected mallctl(\"prof.dump\", ...) failure");
	assert_zu_lt(dump_len, DUMP_BUF_SIZE, "Dump capture buffer overflow");
	dump_buf[dump_len] = '\0';
}

typedef struct frag_rec_s {
	uint64_t age_ns;
	size_t   size;
	size_t   usize;
	unsigned szind;
	unsigned arena_ind;
	uint64_t thr_uid;
} frag_rec_t;

/*
 * Count "f:" records whose request size is `size`; if last is not NULL, it is
 * filled with the last matching record.
 */
static size_t
frag_rec_count(size_t size, frag_rec_t *last) {
	size_t count = 0;
	for (const char *line = dump_buf; line != NULL;
	    line = strchr(line, '\n'), line = (line == NULL) ? NULL : line + 1) {
		frag_rec_t rec;
		if (sscanf(line, "  f: %" FMTu64 " %zu %zu %u %u %" FMTu64,
		        &rec.age_ns, &rec.size, &rec.usize, &rec.szind,
		        &rec.arena_ind, &rec.thr_uid)
		    != 6) {
			continue;
		}
		if (rec.size != size) {
			continue;
		}
		count++;
		if (last != NULL) {
			*last = rec;
		}
	}
	return count;
}

typedef struct frag_util_rec_s {
	unsigned arena_ind;
	unsigned binind;
	size_t   reg_size;
	size_t   slab_size;
	unsigned nregs;
	unsigned n_shards;
	size_t   curslabs;
	size_t   curregs;
	size_t   nonfull_slabs;
} frag_util_rec_t;

static bool
frag_util_rec_find(unsigned arena_ind, unsigned binind, frag_util_rec_t *out) {
	for (const char *line = dump_buf; line != NULL;
	    line = strchr(line, '\n'), line = (line == NULL) ? NULL : line + 1) {
		frag_util_rec_t rec;
		if (sscanf(line, "frag_util: %u %u %zu %zu %u %u %zu %zu %zu",
		        &rec.arena_ind, &rec.binind, &rec.reg_size,
		        &rec.slab_size, &rec.nregs, &rec.n_shards, &rec.curslabs,
		        &rec.curregs, &rec.nonfull_slabs)
		    != 9) {
			continue;
		}
		if (rec.arena_ind != arena_ind || rec.binind != binind) {
			continue;
		}
		*out = rec;
		return true;
	}
	return false;
}

TEST_BEGIN(test_frag_basic) {
	test_skip_if(!config_prof);

	intercepts_install();

	enum { NALLOCS = 3 };
	const size_t size_small = 17;    /* Promoted to a page-sized extent. */
	const size_t size_large = 40033; /* Large size class. */
	void        *small[NALLOCS];
	void        *large[NALLOCS];

	for (unsigned i = 0; i < NALLOCS; i++) {
		small[i] = mallocx(size_small, 0);
		assert_ptr_not_null(small[i], "Unexpected mallocx() failure");
		large[i] = mallocx(size_large, 0);
		assert_ptr_not_null(large[i], "Unexpected mallocx() failure");
	}

	dump();

	/*
	 * Zero-initialized because expect macros do not abort: on a count
	 * mismatch the fields would be read uninitialized otherwise.
	 */
	frag_rec_t rec = {0};
	expect_zu_eq(frag_rec_count(size_small, &rec), NALLOCS,
	    "Wrong live sampled record count");
	expect_zu_eq(rec.usize, sz_s2u(size_small),
	    "Wrong usize in fragmentation record");
	expect_zu_eq(sz_index2size(rec.szind), rec.usize,
	    "szind does not match usize");
	expect_zu_eq(frag_rec_count(size_large, &rec), NALLOCS,
	    "Wrong live sampled record count");
	expect_zu_eq(rec.usize, sz_s2u(size_large),
	    "Wrong usize in fragmentation record");

	/* Free a subset: the corresponding records must disappear. */
	dallocx(small[0], 0);
	dallocx(large[0], 0);
	dallocx(large[1], 0);

	dump();
	expect_zu_eq(frag_rec_count(size_small, NULL), NALLOCS - 1,
	    "Records of freed allocations must disappear");
	expect_zu_eq(frag_rec_count(size_large, NULL), NALLOCS - 2,
	    "Records of freed allocations must disappear");

	for (unsigned i = 1; i < NALLOCS; i++) {
		dallocx(small[i], 0);
	}
	dallocx(large[2], 0);

	dump();
	expect_zu_eq(frag_rec_count(size_small, NULL), 0,
	    "Records of freed allocations must disappear");
	expect_zu_eq(frag_rec_count(size_large, NULL), 0,
	    "Records of freed allocations must disappear");

	intercepts_restore();
}
TEST_END

TEST_BEGIN(test_frag_age_monotonic) {
	test_skip_if(!config_prof);

	intercepts_install();

	const size_t size = 30011;
	void        *p = mallocx(size, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	dump();
	frag_rec_t first = {0};
	expect_zu_eq(frag_rec_count(size, &first), 1, "Missing record");

	dump();
	frag_rec_t second = {0};
	expect_zu_eq(frag_rec_count(size, &second), 1, "Missing record");
	expect_u64_ge(second.age_ns, first.age_ns,
	    "Age must not decrease across dumps");

	dallocx(p, 0);
	intercepts_restore();
}
TEST_END

TEST_BEGIN(test_frag_xallocx_inplace) {
	test_skip_if(!config_prof);

	intercepts_install();

	const size_t size_old = 5 * PAGE;
	const size_t size_new = 4 * PAGE;

	void *p = mallocx(size_old, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	dump();
	expect_zu_eq(frag_rec_count(size_old, NULL), 1, "Missing record");
	expect_zu_eq(frag_rec_count(size_new, NULL), 0, "Unexpected record");

	/* In-place shrink of a large allocation. */
	size_t usize = xallocx(p, size_new, 0, 0);
	expect_zu_eq(usize, size_new, "Unexpected xallocx() result");

	dump();
	expect_zu_eq(frag_rec_count(size_old, NULL), 0,
	    "Old record must disappear after in-place reallocation");
	frag_rec_t rec = {0};
	expect_zu_eq(frag_rec_count(size_new, &rec), 1,
	    "In-place reallocation must be tracked exactly once");
	expect_zu_eq(rec.usize, size_new, "Wrong usize after xallocx()");

	dallocx(p, 0);

	dump();
	expect_zu_eq(frag_rec_count(size_new, NULL), 0,
	    "Records of freed allocations must disappear");

	intercepts_restore();
}
TEST_END

TEST_BEGIN(test_frag_manual_arena_reset) {
	test_skip_if(!config_prof);

	intercepts_install();

	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	assert_d_eq(
	    mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0), 0,
	    "Unexpected mallctl(\"arenas.create\", ...) failure");

	const size_t size = 50021;
	void        *p = mallocx(
	    size, MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	dump();
	frag_rec_t rec = {0};
	expect_zu_eq(frag_rec_count(size, &rec), 1, "Missing record");
	expect_u_eq(rec.arena_ind, arena_ind,
	    "Record must carry the manual arena index");

	/* arena.<i>.reset frees everything, via the prof_free() path. */
	char cmd[64];
	malloc_snprintf(cmd, sizeof(cmd), "arena.%u.reset", arena_ind);
	assert_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	dump();
	expect_zu_eq(frag_rec_count(size, NULL), 0,
	    "Records must disappear after arena reset");

	intercepts_restore();
}
TEST_END

TEST_BEGIN(test_frag_util) {
	test_skip_if(!config_prof);
	/* frag_util records come from bin stats. */
	test_skip_if(!config_stats);

	intercepts_install();

	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	assert_d_eq(
	    mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0), 0,
	    "Unexpected mallctl(\"arenas.create\", ...) failure");

	/*
	 * Slabs can only be populated by unsampled allocations (sampled ones
	 * are promoted out of slabs), so disable sampling while creating the
	 * fragmented bin.
	 */
	bool prof_active_old;
	bool active = false;
	sz = sizeof(prof_active_old);
	assert_d_eq(mallctl("prof.active", (void *)&prof_active_old, &sz,
	                (void *)&active, sizeof(active)),
	    0, "Unexpected mallctl(\"prof.active\", ...) failure");

	const size_t   size = 48;
	const unsigned binind = (unsigned)sz_size2index(size);
	const unsigned nregs = bin_infos[binind].nregs;
	const unsigned nallocs = 2 * nregs;
	void         **ptrs = malloc(nallocs * sizeof(void *));
	assert_ptr_not_null(ptrs, "Unexpected malloc() failure");
	for (unsigned i = 0; i < nallocs; i++) {
		ptrs[i] = mallocx(
		    size, MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE);
		assert_ptr_not_null(ptrs[i], "Unexpected mallocx() failure");
	}
	/* Free all but one region per slab: maximum fragmentation. */
	unsigned nleft = 0;
	for (unsigned i = 0; i < nallocs; i++) {
		if (i % nregs != 0) {
			dallocx(ptrs[i], MALLOCX_TCACHE_NONE);
		} else {
			nleft++;
		}
	}

	active = true;
	assert_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	                sizeof(active)),
	    0, "Unexpected mallctl(\"prof.active\", ...) failure");

	dump();

	frag_util_rec_t rec = {0};
	expect_true(frag_util_rec_find(arena_ind, binind, &rec),
	    "Missing frag_util record for the fragmented bin");
	expect_zu_eq(rec.reg_size, sz_index2size(binind),
	    "Wrong reg_size in frag_util record");
	expect_u_eq(rec.nregs, nregs, "Wrong nregs in frag_util record");
	expect_zu_ge(rec.curslabs, 2, "Expected multiple slabs");
	expect_zu_ge(rec.curregs, nleft, "curregs below live region count");
	expect_zu_lt(rec.curregs, rec.curslabs * rec.nregs,
	    "Expected slab waste (curregs < curslabs * nregs)");

	for (unsigned i = 0; i < nallocs; i += nregs) {
		dallocx(ptrs[i], MALLOCX_TCACHE_NONE);
	}
	free(ptrs);

	/* Restore the original prof.active. */
	assert_d_eq(mallctl("prof.active", NULL, NULL,
	                (void *)&prof_active_old, sizeof(prof_active_old)),
	    0, "Unexpected mallctl(\"prof.active\", ...) failure");

	intercepts_restore();
}
TEST_END

#define STRESS_NTHREADS 4
#define STRESS_NITERS 500
#define STRESS_NLIVE 8

static atomic_b_t stress_stop;

static void *
stress_thd(void *arg) {
	(void)arg;
	void *live[STRESS_NLIVE] = {NULL};
	for (unsigned i = 0; i < STRESS_NITERS; i++) {
		unsigned slot = i % STRESS_NLIVE;
		if (live[slot] != NULL) {
			dallocx(live[slot], 0);
		}
		/* Mix of small (promoted) and large sampled allocations. */
		size_t size = (i % 2 == 0) ? (i % 100 + 1) : (PAGE * (i % 8 + 1));
		live[slot] = mallocx(size, 0);
		assert_ptr_not_null(live[slot], "Unexpected mallocx() failure");
	}
	for (unsigned slot = 0; slot < STRESS_NLIVE; slot++) {
		if (live[slot] != NULL) {
			dallocx(live[slot], 0);
		}
	}
	return NULL;
}

TEST_BEGIN(test_frag_stress) {
	test_skip_if(!config_prof);

	intercepts_install();
	dump_discard = true;

	thd_t thds[STRESS_NTHREADS];
	atomic_store_b(&stress_stop, false, ATOMIC_RELAXED);
	for (unsigned i = 0; i < STRESS_NTHREADS; i++) {
		thd_create(&thds[i], stress_thd, NULL);
	}
	for (unsigned i = 0; i < 10; i++) {
		assert_d_eq(mallctl("prof.dump", NULL, NULL,
		                (void *)&test_filename, sizeof(test_filename)),
		    0, "Unexpected mallctl(\"prof.dump\", ...) failure");
	}
	for (unsigned i = 0; i < STRESS_NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}

	dump_discard = false;
	intercepts_restore();
}
TEST_END

int
main(void) {
	return test(test_frag_basic, test_frag_age_monotonic,
	    test_frag_xallocx_inplace, test_frag_manual_arena_reset,
	    test_frag_util, test_frag_stress);
}
