#include "test/jemalloc_test.h"

static rtree_elm_t *
node_alloc(size_t nelms)
{
	rtree_elm_t *node;

	node = (rtree_elm_t *)calloc(nelms, sizeof(rtree_elm_t));
	assert_ptr_not_null(node, "Unexpected calloc() failure");

	return (node);
}

static void
node_dalloc(rtree_elm_t *node)
{

	free(node);
}

TEST_BEGIN(test_rtree_read_empty)
{
	unsigned i;

	for (i = 1; i <= (sizeof(uintptr_t) << 3); i++) {
		rtree_t rtree;
		assert_false(rtree_new(&rtree, i, node_alloc, node_dalloc),
		    "Unexpected rtree_new() failure");
		assert_ptr_null(rtree_read(&rtree, 0, false),
		    "rtree_read() should return NULL for empty tree");
		rtree_delete(&rtree);
	}
}
TEST_END

#define	NTHREADS	8
#define	MAX_NBITS	18
#define	NITERS		1000
#define	SEED		42

typedef struct {
	unsigned	nbits;
	rtree_t		rtree;
	uint32_t	seed;
} thd_start_arg_t;

static void *
thd_start(void *varg)
{
	thd_start_arg_t *arg = (thd_start_arg_t *)varg;
	sfmt_t	*sfmt;
	extent_t *extent;
	unsigned i;

	sfmt = init_gen_rand(arg->seed);
	extent = (extent_t *)malloc(sizeof(extent));
	assert_ptr_not_null(extent, "Unexpected malloc() failure");

	for (i = 0; i < NITERS; i++) {
		uintptr_t key = (uintptr_t)gen_rand64(sfmt);
		if (i % 2 == 0) {
			rtree_elm_t *elm;

			elm = rtree_elm_acquire(&arg->rtree, key, false, true);
			assert_ptr_not_null(elm,
			    "Unexpected rtree_elm_acquire() failure");
			rtree_elm_write_acquired(elm, extent);
			rtree_elm_release(elm);

			elm = rtree_elm_acquire(&arg->rtree, key, true, false);
			assert_ptr_not_null(elm,
			    "Unexpected rtree_elm_acquire() failure");
			rtree_elm_read_acquired(elm);
			rtree_elm_release(elm);
		} else
			rtree_read(&arg->rtree, key, false);
	}

	free(extent);
	fini_gen_rand(sfmt);
	return (NULL);
}

TEST_BEGIN(test_rtree_concurrent)
{
	thd_start_arg_t arg;
	thd_t thds[NTHREADS];
	sfmt_t *sfmt;
	unsigned i, j;

	sfmt = init_gen_rand(SEED);
	for (i = 1; i < MAX_NBITS; i++) {
		arg.nbits = i;
		assert_false(rtree_new(&arg.rtree, arg.nbits, node_alloc,
		    node_dalloc), "Unexpected rtree_new() failure");
		arg.seed = gen_rand32(sfmt);
		for (j = 0; j < NTHREADS; j++)
			thd_create(&thds[j], thd_start, (void *)&arg);
		for (j = 0; j < NTHREADS; j++)
			thd_join(thds[j], NULL);
		rtree_delete(&arg.rtree);
	}
	fini_gen_rand(sfmt);
}
TEST_END

#undef NTHREADS
#undef MAX_NBITS
#undef NITERS
#undef SEED

TEST_BEGIN(test_rtree_extrema)
{
	unsigned i;
	extent_t extent_a, extent_b;

	for (i = 1; i <= (sizeof(uintptr_t) << 3); i++) {
		rtree_t rtree;
		assert_false(rtree_new(&rtree, i, node_alloc, node_dalloc),
		    "Unexpected rtree_new() failure");

		assert_false(rtree_write(&rtree, 0, &extent_a),
		    "Unexpected rtree_write() failure, i=%u", i);
		assert_ptr_eq(rtree_read(&rtree, 0, true), &extent_a,
		    "rtree_read() should return previously set value, i=%u", i);

		assert_false(rtree_write(&rtree, ~((uintptr_t)0), &extent_b),
		    "Unexpected rtree_write() failure, i=%u", i);
		assert_ptr_eq(rtree_read(&rtree, ~((uintptr_t)0), true),
		    &extent_b,
		    "rtree_read() should return previously set value, i=%u", i);

		rtree_delete(&rtree);
	}
}
TEST_END

TEST_BEGIN(test_rtree_bits)
{
	unsigned i, j, k;

	for (i = 1; i < (sizeof(uintptr_t) << 3); i++) {
		uintptr_t keys[] = {0, 1,
		    (((uintptr_t)1) << (sizeof(uintptr_t)*8-i)) - 1};
		extent_t extent;
		rtree_t rtree;

		assert_false(rtree_new(&rtree, i, node_alloc, node_dalloc),
		    "Unexpected rtree_new() failure");

		for (j = 0; j < sizeof(keys)/sizeof(uintptr_t); j++) {
			assert_false(rtree_write(&rtree, keys[j], &extent),
			    "Unexpected rtree_write() failure");
			for (k = 0; k < sizeof(keys)/sizeof(uintptr_t); k++) {
				assert_ptr_eq(rtree_read(&rtree, keys[k], true),
				    &extent, "rtree_read() should return "
				    "previously set value and ignore "
				    "insignificant key bits; i=%u, j=%u, k=%u, "
				    "set key=%#"FMTxPTR", get key=%#"FMTxPTR, i,
				    j, k, keys[j], keys[k]);
			}
			assert_ptr_null(rtree_read(&rtree,
			    (((uintptr_t)1) << (sizeof(uintptr_t)*8-i)), false),
			    "Only leftmost rtree leaf should be set; "
			    "i=%u, j=%u", i, j);
			rtree_clear(&rtree, keys[j]);
		}

		rtree_delete(&rtree);
	}
}
TEST_END

TEST_BEGIN(test_rtree_random)
{
	unsigned i;
	sfmt_t *sfmt;
#define	NSET 16
#define	SEED 42

	sfmt = init_gen_rand(SEED);
	for (i = 1; i <= (sizeof(uintptr_t) << 3); i++) {
		uintptr_t keys[NSET];
		extent_t extent;
		unsigned j;
		rtree_t rtree;
		rtree_elm_t *elm;

		assert_false(rtree_new(&rtree, i, node_alloc, node_dalloc),
		    "Unexpected rtree_new() failure");

		for (j = 0; j < NSET; j++) {
			keys[j] = (uintptr_t)gen_rand64(sfmt);
			elm = rtree_elm_acquire(&rtree, keys[j], false, true);
			assert_ptr_not_null(elm,
			    "Unexpected rtree_elm_acquire() failure");
			rtree_elm_write_acquired(elm, &extent);
			rtree_elm_release(elm);
			assert_ptr_eq(rtree_read(&rtree, keys[j], true),
			    &extent,
			    "rtree_read() should return previously set value");
		}
		for (j = 0; j < NSET; j++) {
			assert_ptr_eq(rtree_read(&rtree, keys[j], true),
			    &extent, "rtree_read() should return previously "
			    "set value, j=%u", j);
		}

		for (j = 0; j < NSET; j++) {
			rtree_clear(&rtree, keys[j]);
			assert_ptr_null(rtree_read(&rtree, keys[j], true),
			    "rtree_read() should return previously set value");
		}
		for (j = 0; j < NSET; j++) {
			assert_ptr_null(rtree_read(&rtree, keys[j], true),
			    "rtree_read() should return previously set value");
		}

		rtree_delete(&rtree);
	}
	fini_gen_rand(sfmt);
#undef NSET
#undef SEED
}
TEST_END

int
main(void)
{

	return (test(
	    test_rtree_read_empty,
	    test_rtree_concurrent,
	    test_rtree_extrema,
	    test_rtree_bits,
	    test_rtree_random));
}
