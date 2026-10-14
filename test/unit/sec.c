#include "test/jemalloc_test.h"

#include "jemalloc/internal/sec.h"

typedef struct test_data_s test_data_t;
struct test_data_s {
	/*
	 * Must be the first member -- we convert back and forth between the
	 * test_data_t and the sec_t;
	 */
	sec_t   sec;
	base_t *base;
};

static void
edata_init_test(edata_t *edata) {
	memset(edata, 0, sizeof(*edata));
}

static void
edata_init_test_size(edata_t *edata, size_t size) {
	edata_init_test(edata);
	edata_size_set(edata, size);
}

static void
test_data_init(tsdn_t *tsdn, test_data_t *tdata, const sec_opts_t *opts) {
	tdata->base = base_new(TSDN_NULL, /* ind */ 123,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);

	bool err = sec_init(tsdn, &tdata->sec, tdata->base, opts);
	assert_false(err, "Unexpected initialization failure");
	if (tdata->sec.opts.nshards > 0) {
		assert_u_ge(tdata->sec.npsizes, 0,
		    "Zero size classes allowed for caching");
	}
}

static void
destroy_test_data(tsdn_t *tsdn, test_data_t *tdata) {
	/* There is no destroy sec to delete the bins ?! */
	base_delete(tsdn, tdata->base);
}

static uint8_t
test_sec_shard(tsdn_t *tsdn, sec_t *sec) {
	if (tsdn_null(tsdn) || sec->opts.nshards <= 1) {
		return 0;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	return sec_shard_pick(tsd, sec, tsd_sec_shardp_get(tsd));
}

static edata_t *
sec_test_alloc(tsdn_t *tsdn, sec_t *sec, size_t size) {
	return sec_alloc(tsdn, sec, size, test_sec_shard(tsdn, sec));
}

static void
sec_test_fill(tsdn_t *tsdn, sec_t *sec, size_t size,
    edata_list_active_t *result, size_t nallocs) {
	sec_fill(tsdn, sec, size, result, nallocs, test_sec_shard(tsdn, sec));
}

static void
sec_test_dalloc(tsdn_t *tsdn, sec_t *sec,
    edata_list_active_t *dalloc_list) {
	sec_dalloc(tsdn, sec, dalloc_list, test_sec_shard(tsdn, sec));
}

TEST_BEGIN(test_max_nshards_option_zero) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 0;
	opts.max_alloc = PAGE;
	opts.max_bytes = 512 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_t *edata = sec_size_supported(&tdata.sec, PAGE)
	    ? sec_test_alloc(tsdn, &tdata.sec, PAGE) : NULL;
	expect_ptr_null(edata, "SEC should be disabled when nshards==0");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_max_alloc_option_too_small) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 512 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_t *edata = sec_size_supported(&tdata.sec, 3 * PAGE)
	    ? sec_test_alloc(tsdn, &tdata.sec, 3 * PAGE) : NULL;
	expect_ptr_null(edata, "max_alloc is 2*PAGE, should not alloc 3*PAGE");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_fill) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 4 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Fill the cache with two extents */
	sec_stats_t         stats = {0};
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1, edata2;
	edata_init_test_size(&edata1, PAGE);
	edata_init_test_size(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata1);
	edata_list_active_append(&allocs, &edata2);
	sec_test_fill(tsdn, &tdata.sec, PAGE, &allocs, 2);
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 2 * PAGE, "SEC should have what we filled");
	expect_true(edata_list_active_empty(&allocs),
	    "extents should be consumed by sec");

	/* Try to overfill and confirm that max_bytes is respected. */
	stats.bytes = 0;
	edata_t edata5, edata4, edata3;
	edata_init_test_size(&edata3, PAGE);
	edata_init_test_size(&edata4, PAGE);
	edata_init_test_size(&edata5, PAGE);
	edata_list_active_append(&allocs, &edata3);
	edata_list_active_append(&allocs, &edata4);
	edata_list_active_append(&allocs, &edata5);
	sec_test_fill(tsdn, &tdata.sec, PAGE, &allocs, 3);
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(
	    stats.bytes, opts.max_bytes, "SEC can't have more than max_bytes");
	expect_false(edata_list_active_empty(&allocs), "Not all should fit");
	expect_zu_eq(stats.total.noverfills, 1, "Expected one overfill");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_alloc) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 4 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Alloc from empty cache returns NULL */
	edata_t *edata = sec_test_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC is empty");

	/* Place two extents into the sec */
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1, edata2;
	edata_init_test_size(&edata1, PAGE);
	edata_list_active_append(&allocs, &edata1);
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(edata_list_active_empty(&allocs), "");
	edata_init_test_size(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata2);
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(edata_list_active_empty(&allocs), "");

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 2 * PAGE,
	    "After fill bytes should reflect what is in the cache");
	stats.bytes = 0;

	/* Most recently cached extent should be used on alloc */
	edata = sec_test_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_eq(edata, &edata2, "edata2 is most recently used");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, PAGE, "One more item left in the cache");
	stats.bytes = 0;

	/* Alloc can still get extents from cache */
	edata = sec_test_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_eq(edata, &edata1, "SEC is not empty");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "No more items after last one is popped");

	/* And cache is empty again */
	edata = sec_test_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC is empty");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_dalloc) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Return one extent into the cache */
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1;
	edata_init_test_size(&edata1, PAGE);
	edata_list_active_append(&allocs, &edata1);

	/* SEC is empty, we return one pointer to it */
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	/* Return one more extent, so that we are at the limit */
	edata_t edata2;
	edata_init_test_size(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata2);
	/* Sec can take one more as well and we will be exactly at max_bytes */
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, opts.max_bytes, "Size should match deallocs");
	stats.bytes = 0;

	/*
	 * We are at max_bytes.  Now, we dalloc one more pointer and we go above
	 * the limit.  This will force flush to 3/4 of max_bytes and given that
	 * we have max of 2 pages, we will have to flush two. We will not flush
	 * the one given in the input as it is the most recently used.
	 */
	edata_t edata3;
	edata_init_test_size(&edata3, PAGE);
	edata_list_active_append(&allocs, &edata3);
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_false(
	    edata_list_active_empty(&allocs), "extents should NOT be consumed");
	expect_ptr_ne(
	    edata_list_active_first(&allocs), &edata3, "edata3 is MRU");
	expect_ptr_ne(
	    edata_list_active_last(&allocs), &edata3, "edata3 is MRU");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(PAGE, stats.bytes, "Should have flushed");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_pinned_stats) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t pinned, unpinned;
	edata_init_test_size(&pinned, PAGE);
	edata_pinned_set(&pinned, true);
	edata_init_test_size(&unpinned, PAGE);

	edata_list_active_append(&allocs, &pinned);
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	edata_list_active_append(&allocs, &unpinned);
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 2 * PAGE, "");
	expect_zu_eq(stats.bytes_pinned, PAGE, "");
	sec_pszind_stats_t pszind_stats = {0};
	sec_stats_merge_pszind(
	    tsdn, &tdata.sec, sz_psz2ind(PAGE), &pszind_stats);
	expect_zu_eq(pszind_stats.nextents, 2, "");
	expect_zu_eq(pszind_stats.bytes, 2 * PAGE, "");
	expect_zu_eq(pszind_stats.nextents_pinned, 1, "");
	expect_zu_eq(pszind_stats.bytes_pinned, PAGE, "");

	memset(&stats, 0, sizeof(stats));
	expect_ptr_eq(sec_test_alloc(tsdn, &tdata.sec, PAGE), &unpinned,
	    "Unpinned extent is the most recently cached");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, PAGE, "");
	expect_zu_eq(stats.bytes_pinned, PAGE, "");
	memset(&pszind_stats, 0, sizeof(pszind_stats));
	sec_stats_merge_pszind(
	    tsdn, &tdata.sec, sz_psz2ind(PAGE), &pszind_stats);
	expect_zu_eq(pszind_stats.nextents, 1, "");
	expect_zu_eq(pszind_stats.bytes, PAGE, "");
	expect_zu_eq(pszind_stats.nextents_pinned, 1, "");
	expect_zu_eq(pszind_stats.bytes_pinned, PAGE, "");

	memset(&stats, 0, sizeof(stats));
	expect_ptr_eq(sec_test_alloc(tsdn, &tdata.sec, PAGE), &pinned,
	    "Pinned extent should be tracked until allocated");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "");
	expect_zu_eq(stats.bytes_pinned, 0, "");

	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_max_bytes_too_low) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = 4 * PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Return one extent into the cache. Item is too big */
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1;
	edata_init_test_size(&edata1, 3 * PAGE);
	edata_list_active_append(&allocs, &edata1);

	/* SEC is empty, we return one pointer to it */
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_false(
	    edata_list_active_empty(&allocs), "extents should not be consumed");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_flush) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = 4 * PAGE;
	opts.max_bytes = 1024 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* We put in 10 one-page extents, and 10 four-page extents */
	edata_list_active_t allocs1;
	edata_list_active_t allocs4;
	edata_list_active_init(&allocs1);
	edata_list_active_init(&allocs4);
	enum { NALLOCS = 10 };
	edata_t edata1[NALLOCS];
	edata_t edata4[NALLOCS];
	for (int i = 0; i < NALLOCS; i++) {
		edata_init_test_size(&edata1[i], PAGE);
		edata_init_test_size(&edata4[i], 4 * PAGE);

		edata_list_active_append(&allocs1, &edata1[i]);
		sec_test_dalloc(tsdn, &tdata.sec, &allocs1);
		edata_list_active_append(&allocs4, &edata4[i]);
		sec_test_dalloc(tsdn, &tdata.sec, &allocs4);
	}

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(
	    stats.bytes, 10 * 5 * PAGE, "SEC should have what we filled");
	stats.bytes = 0;

	expect_true(edata_list_active_empty(&allocs1), "");
	sec_flush(tsdn, &tdata.sec, &allocs1);
	expect_false(edata_list_active_empty(&allocs1), "");

	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "SEC should be empty");
	stats.bytes = 0;
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_stats) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	opts.nshards = 1;
	opts.max_alloc = PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1;
	edata_init_test_size(&edata1, PAGE);
	edata_list_active_append(&allocs, &edata1);

	/* SEC is empty alloc fails. nmisses==1 */
	edata_t *edata = sec_test_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC should be empty");

	/* SEC is empty, we return one pointer to it. ndalloc_noflush=1 */
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	edata_t edata2;
	edata_init_test_size(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata2);
	/* Sec can take one more, so ndalloc_noflush=2 */
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	sec_stats_t stats;
	memset(&stats, 0, sizeof(sec_stats_t));
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, opts.max_bytes, "Size should match deallocs");
	expect_zu_eq(stats.total.ndalloc_noflush, 2, "");
	expect_zu_eq(stats.total.nmisses, 1, "");

	memset(&stats, 0, sizeof(sec_stats_t));

	/*
	 * We are at max_bytes.  Now, we dalloc one more pointer and we go above
	 * the limit.  This will force flush, so ndalloc_flush = 1.
	 */
	edata_t edata3;
	edata_init_test_size(&edata3, PAGE);
	edata_list_active_append(&allocs, &edata3);
	sec_test_dalloc(tsdn, &tdata.sec, &allocs);
	expect_false(
	    edata_list_active_empty(&allocs), "extents should NOT be consumed");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(PAGE, stats.bytes, "Should have flushed");
	expect_zu_eq(stats.total.ndalloc_flush, 1, "");
	memset(&stats, 0, sizeof(sec_stats_t));
	destroy_test_data(tsdn, &tdata);
}
TEST_END

#define NOPS_PER_THREAD 100
#define NPREFILL 32

typedef struct {
	sec_t              *sec;
	uint8_t             preferred_shard;
	size_t              nallocs;
	size_t              nallocs_fail;
	size_t              ndallocs;
	size_t              ndallocs_fail;
	edata_list_active_t fill_list;
	size_t              fill_list_sz;
	edata_t            *edata[NOPS_PER_THREAD];
} trylock_test_arg_t;

static void *
thd_trylock_test(void *varg) {
	trylock_test_arg_t *arg = (trylock_test_arg_t *)varg;
	tsd_t              *tsd = tsd_fetch();
	tsdn_t             *tsdn = tsd_tsdn(tsd);

	/* Set the preferred shard for this thread */
	uint8_t *shard_idx = tsd_sec_shardp_get(tsd);
	*shard_idx = arg->preferred_shard;

	/* Fill the shard with some extents */
	sec_test_fill(tsdn, arg->sec, PAGE, &arg->fill_list, arg->fill_list_sz);
	expect_true(edata_list_active_empty(&arg->fill_list), "");

	for (unsigned i = 0; i < NOPS_PER_THREAD; i++) {
		/* Try to allocate from SEC */
		arg->edata[i] = sec_test_alloc(tsdn, arg->sec, PAGE);
		if (arg->edata[i] != NULL) {
			expect_zu_eq(edata_size_get(arg->edata[i]), PAGE, "");
		}
	}

	for (unsigned i = 0; i < NOPS_PER_THREAD; i++) {
		if (arg->edata[i] != NULL) {
			edata_list_active_t list;
			edata_list_active_init(&list);
			arg->nallocs++;
			edata_list_active_append(&list, arg->edata[i]);
			expect_zu_eq(edata_size_get(arg->edata[i]), PAGE, "");
			sec_test_dalloc(tsdn, arg->sec, &list);
			if (edata_list_active_empty(&list)) {
				arg->ndallocs++;
			} else {
				arg->ndallocs_fail++;
			}
		} else {
			arg->nallocs_fail++;
		}
	}

	return NULL;
}

TEST_BEGIN(test_sec_multishard) {
	test_data_t tdata;
	sec_opts_t  opts = {0};
	enum { NSHARDS = 2 };
	enum { NTHREADS = NSHARDS * 16 };
	opts.nshards = NSHARDS;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 64 * NTHREADS * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Create threads with different preferred shards */
	thd_t              thds[NTHREADS];
	trylock_test_arg_t args[NTHREADS];

	edata_t all_edatas[NPREFILL * NTHREADS];

	for (unsigned i = 0; i < NTHREADS; i++) {
		edata_list_active_init(&args[i].fill_list);
		for (unsigned j = 0; j < NPREFILL; ++j) {
			size_t ind = i * NPREFILL + j;
			edata_init_test_size(&all_edatas[ind], PAGE);
			edata_list_active_append(
			    &args[i].fill_list, &all_edatas[ind]);
		}
		args[i].fill_list_sz = NPREFILL;
		args[i].sec = &tdata.sec;
		args[i].preferred_shard = i % opts.nshards;
		args[i].nallocs = 0;
		args[i].nallocs_fail = 0;
		args[i].ndallocs = 0;
		args[i].ndallocs_fail = 0;
		memset(
		    &args[i].edata[0], 0, NOPS_PER_THREAD * sizeof(edata_t *));
		thd_create(&thds[i], thd_trylock_test, &args[i]);
	}

	for (unsigned i = 0; i < NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}

	/* Wait for all threads to complete */
	size_t total_allocs = 0;
	size_t total_dallocs = 0;
	size_t total_allocs_fail = 0;
	for (unsigned i = 0; i < NTHREADS; i++) {
		total_allocs += args[i].nallocs;
		total_dallocs += args[i].ndallocs;
		total_allocs_fail += args[i].nallocs_fail;
	}

	/* We must have at least some hits */
	expect_zu_gt(total_allocs, 0, "");
	/*
	 * We must have at least some successful dallocs by design (max_bytes is
	 * big enough).
	 */
	expect_zu_gt(total_dallocs, 0, "");

	/* Get final stats to verify that hits and misses are accurate */
	sec_stats_t stats = {0};
	memset(&stats, 0, sizeof(sec_stats_t));
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.total.nhits, total_allocs, "");
	expect_zu_eq(stats.total.nmisses, total_allocs_fail, "");

	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_trim_disabled) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_t tdata;
	sec_opts_t opts = HPA_SEC_OPTS_DEFAULT;
	expect_false(opts.experimental_trim, "HPA SEC trimming defaults off");
	sec_opts_t pac_opts = PAC_SEC_OPTS_DEFAULT;
	expect_false(pac_opts.experimental_trim, "PAC keeps its original policy");
	opts.nshards = 1;
	opts.max_alloc = PAGE;
	opts.max_bytes = 8 * PAGE;
	test_data_init(tsdn, &tdata, &opts);
	expect_ptr_null(sec_alloc(tsdn, &tdata.sec, PAGE, 0), "Initial miss");
	edata_t edatas[9];
	edata_list_active_t list;
	edata_list_active_init(&list);
	for (size_t i = 0; i < 9; i++) {
		edata_init_test_size(&edatas[i], PAGE);
		edata_pinned_set(&edatas[i], i % 2 == 0);
		edata_list_active_append(&list, &edatas[i]);
		sec_dalloc(tsdn, &tdata.sec, &list, 0);
	}
	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 6 * PAGE, "Original overflow retains 3/4");
	expect_zu_eq(stats.bytes_pinned, 3 * PAGE, "Pinned overflow accounting");
	for (size_t i = 0; i < 3; i++) {
		edata_t *edata = edata_list_active_first(&list);
		assert_ptr_eq(edata, &edatas[i], "Oldest overflow extents first");
		edata_list_active_remove(&list, edata);
	}
	sec_trim(tsdn, &tdata.sec, &list);
	expect_true(edata_list_active_empty(&list),
	    "Disabled passes leave SEC alone");
	expect_ptr_eq(sec_alloc(tsdn, &tdata.sec, PAGE, 0), &edatas[8],
	    "Cache hits still work");
	sec_flush(tsdn, &tdata.sec, &list);
	stats = (sec_stats_t){0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "Explicit flushing still drains SEC");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_trim_overflow) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_t tdata;
	sec_opts_t opts = {1, PAGE, 8 * PAGE, true};
	test_data_init(tsdn, &tdata, &opts);
	edata_t edatas[9];
	edata_list_active_t list;
	edata_list_active_init(&list);
	for (size_t i = 0; i < 9; i++) {
		edata_init_test_size(&edatas[i], PAGE);
		edata_pinned_set(&edatas[i], i % 2 == 0);
		edata_list_active_append(&list, &edatas[i]);
		sec_test_dalloc(tsdn, &tdata.sec, &list);
		if (i < 8) {
			expect_true(edata_list_active_empty(&list), "Below capacity");
		}
		if (i == 7) {
			sec_trim(tsdn, &tdata.sec, &list);
			expect_true(edata_list_active_empty(&list),
			    "First pass preserves inventory at capacity");
			expect_zu_eq(tdata.sec.bins[0].low_water_bytes, 8 * PAGE,
			    "Establish a nonzero watermark before overflow");
		}
	}
	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 6 * PAGE,
	    "Enabled trimming preserves 3/4 overflow retention");
	expect_zu_eq(stats.bytes_pinned, 3 * PAGE, "Pinned bytes track retention");
	expect_zu_eq(tdata.sec.bins[0].low_water_bytes, 6 * PAGE,
	    "Overflow lowers the watermark to retained inventory");
	for (size_t i = 0; i < 3; i++) {
		edata_t *edata = edata_list_active_first(&list);
		assert_ptr_eq(edata, &edatas[i], "Oldest returns leave first");
		edata_list_active_remove(&list, edata);
	}
	expect_true(edata_list_active_empty(&list), "Three cold extents flushed");
	expect_ptr_eq(sec_test_alloc(tsdn, &tdata.sec, PAGE), &edatas[8],
	    "Newest return is retained");
	sec_flush(tsdn, &tdata.sec, &list);
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_trim_low_water) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_t tdata;
	sec_opts_t opts = {1, PAGE, 8 * PAGE, true};
	test_data_init(tsdn, &tdata, &opts);
	edata_t edatas[8];
	edata_list_active_t list;
	edata_list_active_init(&list);
	/* An actual miss precedes a speculative fill. */
	expect_ptr_null(sec_alloc(tsdn, &tdata.sec, PAGE, 0), "Empty bin");
	for (size_t i = 0; i < 8; i++) {
		edata_init_test_size(&edatas[i], PAGE);
		edata_pinned_set(&edatas[i], i % 2 == 0);
		edata_list_active_append(&list, &edatas[i]);
	}
	sec_fill(tsdn, &tdata.sec, PAGE, &list, 8, 0);
	sec_trim(tsdn, &tdata.sec, &list);
	expect_true(edata_list_active_empty(&list), "First observation preserves fresh stock");
	edata_t *first = sec_alloc(tsdn, &tdata.sec, PAGE, 0);
	edata_t *second = sec_alloc(tsdn, &tdata.sec, PAGE, 0);
	assert_ptr_eq(first, &edatas[0], "");
	assert_ptr_eq(second, &edatas[1], "");
	edata_list_active_append(&list, first);
	sec_dalloc(tsdn, &tdata.sec, &list, 0);
	edata_list_active_append(&list, second);
	sec_dalloc(tsdn, &tdata.sec, &list, 0);
	sec_trim(tsdn, &tdata.sec, &list);
	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 5 * PAGE, "Return half the observed minimum");
	expect_zu_eq(stats.bytes_pinned, 3 * PAGE, "Pinned accounting after trim");
	expect_zu_eq(stats.total.nhits, 2, "Cumulative hits survive trim");
	expect_zu_eq(stats.total.nmisses, 1, "Cumulative misses survive trim");
	for (size_t i = 8; i-- > 5;) {
		edata_t *edata = edata_list_active_first(&list);
		assert_ptr_eq(edata, &edatas[i], "Cold tail first");
		edata_list_active_remove(&list, edata);
	}
	for (size_t remaining = 5; remaining > 0;) {
		sec_trim(tsdn, &tdata.sec, &list);
		remaining /= 2;
		expect_zu_eq(atomic_load_zu(&tdata.sec.bins[0].bytes_cur,
		    ATOMIC_RELAXED), remaining * PAGE, "Reserve halves to zero");
	}
	expect_ptr_eq(edata_list_active_first(&list), &edatas[4], "Cold tail first");
	expect_ptr_eq(edata_list_active_last(&list), second, "Newest return last");
	stats = (sec_stats_t){0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "Empty bin");
	expect_zu_eq(stats.bytes_pinned, 0, "No stale pinned bytes");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_trim_half_low_water_rounding) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	/* Include a multi-page extent whose half is not a whole extent. */
	const size_t size = 3 * PAGE;
	for (size_t n = 2; n <= 6; n += 2) {
		test_data_t tdata;
		sec_opts_t opts = {1, size, 8 * size, true};
		test_data_init(tsdn, &tdata, &opts);
		edata_t edatas[6];
		edata_list_active_t list;
		edata_list_active_init(&list);
		expect_ptr_null(sec_alloc(tsdn, &tdata.sec, size, 0), "Record demand");
		for (size_t i = 0; i < n; i++) {
			edata_init_test_size(&edatas[i], size);
			edata_pinned_set(&edatas[i], i % 2 == 0);
			edata_list_active_append(&list, &edatas[i]);
		}
		sec_fill(tsdn, &tdata.sec, size, &list, n, 0);
		sec_trim(tsdn, &tdata.sec, &list);
		expect_true(edata_list_active_empty(&list), "Fresh refill survives");
		edata_t *live = sec_alloc(tsdn, &tdata.sec, size, 0);
		assert_ptr_eq(live, &edatas[0], "Consume one extent");
		edata_list_active_append(&list, live);
		sec_dalloc(tsdn, &tdata.sec, &list, 0);
		sec_trim(tsdn, &tdata.sec, &list);
		size_t nreturned = n / 2;
		for (size_t i = 0; i < nreturned; i++) {
			edata_t *edata = edata_list_active_first(&list);
			assert_ptr_eq(edata, &edatas[n - 1 - i], "Cold extents return first");
			edata_list_active_remove(&list, edata);
		}
		expect_true(edata_list_active_empty(&list),
		    "Odd low water rounds the returned amount up to whole extents");
		sec_stats_t stats = {0};
		sec_stats_merge(tsdn, &tdata.sec, &stats);
		expect_zu_eq(stats.bytes, (n - nreturned) * size, "Reserve retained");
		expect_zu_eq(stats.bytes_pinned, ((n - nreturned + 1) / 2) * size,
		    "Pinned accounting includes retained reserve");
		sec_bin_t *bin = &tdata.sec.bins[sz_psz2ind(size)];
		expect_zu_eq(bin->low_water_bytes, stats.bytes, "Reset to retained bytes");
		for (size_t remaining = n - nreturned; remaining > 0;) {
			sec_trim(tsdn, &tdata.sec, &list);
			remaining /= 2;
			stats = (sec_stats_t){0};
			sec_stats_merge(tsdn, &tdata.sec, &stats);
			expect_zu_eq(stats.bytes, remaining * size,
			    "Repeated passes drain even a single multi-page extent");
			expect_zu_eq(stats.bytes_pinned, ((remaining + 1) / 2) * size,
			    "Pinned accounting follows gradual reclamation");
		}
		destroy_test_data(tsdn, &tdata);
	}
}
TEST_END

TEST_BEGIN(test_sec_trim_return_only_and_flush) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_t tdata;
	sec_opts_t opts = {2, 2 * PAGE, 8 * PAGE, true};
	test_data_init(tsdn, &tdata, &opts);
	edata_t edatas[2];
	edata_list_active_t list;
	edata_list_active_init(&list);
	for (uint8_t shard = 0; shard < 2; shard++) {
		edata_init_test_size(&edatas[shard], (shard + 1) * PAGE);
		edata_list_active_append(&list, &edatas[shard]);
		sec_dalloc(tsdn, &tdata.sec, &list, shard);
	}
	sec_trim(tsdn, &tdata.sec, &list);
	expect_true(edata_list_active_empty(&list), "First pass observes returns");
	sec_trim(tsdn, &tdata.sec, &list);
	expect_ptr_eq(edata_list_active_first(&list), &edatas[0],
	    "Second pass returns the final extent in each bin");
	expect_ptr_eq(edata_list_active_last(&list), &edatas[1],
	    "Visit every shard and size bin");
	edata_list_active_remove(&list, &edatas[0]);
	edata_list_active_remove(&list, &edatas[1]);
	expect_ptr_null(sec_alloc(tsdn, &tdata.sec, PAGE, 0), "Record demand");
	edata_list_active_append(&list, &edatas[0]);
	sec_fill(tsdn, &tdata.sec, PAGE, &list, 1, 0);
	sec_flush(tsdn, &tdata.sec, &list);
	expect_zu_eq(tdata.sec.bins[0].low_water_bytes, 0, "Flush resets minimum");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

#define RECLAIM_TEST_EXTENTS 128
typedef struct {
	sec_bin_t *bin;
	atomic_b_t held;
	atomic_b_t release;
} trim_locked_bin_t;

static void *
trim_locked_bin_worker(void *arg) {
	trim_locked_bin_t *data = arg;
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	malloc_mutex_lock(tsdn, &data->bin->mtx);
	atomic_store_b(&data->held, true, ATOMIC_RELEASE);
	while (!atomic_load_b(&data->release, ATOMIC_ACQUIRE)) {
		sleep_ns(1000 * 1000);
	}
	malloc_mutex_unlock(tsdn, &data->bin->mtx);
	return NULL;
}

TEST_BEGIN(test_sec_trim_contended_bin) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_t tdata;
	sec_opts_t opts = {1, PAGE, 8 * PAGE, true};
	test_data_init(tsdn, &tdata, &opts);
	edata_t edata;
	edata_init_test_size(&edata, PAGE);
	edata_list_active_t list;
	edata_list_active_init(&list);
	edata_list_active_append(&list, &edata);
	sec_dalloc(tsdn, &tdata.sec, &list, 0);
	trim_locked_bin_t data = {.bin = &tdata.sec.bins[0],
	    .held = ATOMIC_INIT(false), .release = ATOMIC_INIT(false)};
	thd_t thread;
	thd_create(&thread, trim_locked_bin_worker, &data);
	while (!atomic_load_b(&data.held, ATOMIC_ACQUIRE)) {
		sleep_ns(1000 * 1000);
	}
	sec_trim(tsdn, &tdata.sec, &list);
	expect_true(edata_list_active_empty(&list), "Contended bin skipped");
	atomic_store_b(&data.release, true, ATOMIC_RELEASE);
	thd_join(thread, NULL);
	sec_trim(tsdn, &tdata.sec, &list);
	expect_true(edata_list_active_empty(&list), "Retry establishes low watermark");
	sec_trim(tsdn, &tdata.sec, &list);
	expect_ptr_eq(edata_list_active_first(&list), &edata, "Next pass drains bin");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

#define RECLAIM_TEST_OPS 10000
typedef struct {
	sec_t *sec;
	edata_t *edatas;
	atomic_u_t busy[RECLAIM_TEST_EXTENTS];
} reclaim_test_data_t;

static void *
reclaim_test_worker(void *arg) {
	reclaim_test_data_t *data = arg;
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	for (size_t i = 0; i < RECLAIM_TEST_OPS; i++) {
		edata_t *edata = sec_alloc(tsdn, data->sec, PAGE, 0);
		if (edata == NULL) {
			continue;
		}
		size_t ind = (size_t)(edata - data->edatas);
		assert_zu_lt(ind, RECLAIM_TEST_EXTENTS, "Foreign extent");
		expect_u_eq(atomic_fetch_add_u(&data->busy[ind], 1,
		    ATOMIC_RELAXED), 0, "Extent handed to two live owners");
		expect_u_eq(atomic_fetch_sub_u(&data->busy[ind], 1,
		    ATOMIC_RELAXED), 1, "Live extent reclaimed");
		edata_list_active_t list;
		edata_list_active_init(&list);
		edata_list_active_append(&list, edata);
		sec_dalloc(tsdn, data->sec, &list, 0);
		expect_true(edata_list_active_empty(&list), "Inventory fits capacity");
	}
	return NULL;
}

TEST_BEGIN(test_sec_concurrent_trim_fill) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_t tdata;
	sec_opts_t opts = {2, PAGE, RECLAIM_TEST_EXTENTS * PAGE, true};
	test_data_init(tsdn, &tdata, &opts);
	edata_t edatas[RECLAIM_TEST_EXTENTS];
	reclaim_test_data_t data = {.sec = &tdata.sec, .edatas = edatas};
	edata_list_active_t list;
	edata_list_active_init(&list);
	for (size_t i = 0; i < RECLAIM_TEST_EXTENTS; i++) {
		edata_init_test_size(&edatas[i], PAGE);
		atomic_store_u(&data.busy[i], 0, ATOMIC_RELAXED);
		edata_list_active_append(&list, &edatas[i]);
	}
	sec_fill(tsdn, &tdata.sec, PAGE, &list, RECLAIM_TEST_EXTENTS, 1);
	thd_t threads[4];
	for (size_t i = 0; i < 4; i++) {
		thd_create(&threads[i], reclaim_test_worker, &data);
	}
	for (size_t i = 0; i < RECLAIM_TEST_OPS; i++) {
		sec_trim(tsdn, &tdata.sec, &list);
		edata_t *edata;
		while ((edata = edata_list_active_first(&list)) != NULL) {
			edata_list_active_remove(&list, edata);
			size_t ind = (size_t)(edata - edatas);
			assert_zu_lt(ind, RECLAIM_TEST_EXTENTS, "Foreign extent");
			expect_u_eq(atomic_load_u(&data.busy[ind], ATOMIC_RELAXED),
			    0, "Reclaim must not detach a live extent");
			edata_list_active_t fill;
			edata_list_active_init(&fill);
			edata_list_active_append(&fill, edata);
			sec_fill(tsdn, &tdata.sec, PAGE, &fill, 1, 1);
			expect_true(edata_list_active_empty(&fill), "");
		}
	}
	for (size_t i = 0; i < 4; i++) {
		thd_join(threads[i], NULL);
	}
	sec_flush(tsdn, &tdata.sec, &list);
	bool seen[RECLAIM_TEST_EXTENTS] = {false};
	size_t count = 0;
	edata_t *edata;
	while ((edata = edata_list_active_first(&list)) != NULL) {
		edata_list_active_remove(&list, edata);
		size_t ind = (size_t)(edata - edatas);
		assert_zu_lt(ind, RECLAIM_TEST_EXTENTS, "Foreign extent");
		expect_false(seen[ind], "Duplicate inventory after concurrent operations");
		seen[ind] = true;
		count++;
	}
	expect_zu_eq(count, RECLAIM_TEST_EXTENTS, "No lost metadata");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

int
main(void) {
	return test(test_max_nshards_option_zero,
	    test_max_alloc_option_too_small, test_sec_fill, test_sec_alloc,
	    test_sec_dalloc, test_sec_pinned_stats, test_max_bytes_too_low,
	    test_sec_flush, test_sec_stats, test_sec_multishard,
	    test_sec_trim_disabled, test_sec_trim_overflow,
	    test_sec_trim_low_water,
	    test_sec_trim_half_low_water_rounding,
	    test_sec_trim_return_only_and_flush, test_sec_trim_contended_bin,
	    test_sec_concurrent_trim_fill);
}
