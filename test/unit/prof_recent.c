#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_recent.h"

/* As specified in the shell script */
#define OPT_ALLOC_MAX	3

/* Invariant before and after every test (when config_prof is on) */
static void confirm_prof_setup(tsd_t *tsd) {
	/* Options */
	assert_true(opt_prof, "opt_prof not on");
	assert_true(opt_prof_active, "opt_prof_active not on");
	assert_zd_eq(opt_prof_recent_alloc_max, OPT_ALLOC_MAX,
	    "opt_prof_recent_alloc_max not set correctly");

	/* Dynamics */
	assert_true(prof_active, "prof_active not on");
	assert_zd_eq(prof_recent_alloc_max_ctl_read(tsd), OPT_ALLOC_MAX,
	    "prof_recent_alloc_max not set correctly");
}

TEST_BEGIN(test_confirm_setup) {
	test_skip_if(!config_prof);
	confirm_prof_setup(tsd_fetch());
}
TEST_END

TEST_BEGIN(test_prof_recent_off) {
	test_skip_if(config_prof);

	const ssize_t past_ref = 0, future_ref = 0;
	const size_t len_ref = sizeof(ssize_t);

	ssize_t past = past_ref, future = future_ref;
	size_t len = len_ref;

#define ASSERT_SHOULD_FAIL(opt, a, b, c, d) do {			\
	assert_d_eq(mallctl("experimental.prof_recent." opt, a, b, c,	\
	    d), ENOENT, "Should return ENOENT when config_prof is off");\
	assert_zd_eq(past, past_ref, "output was touched");		\
	assert_zu_eq(len, len_ref, "output length was touched");	\
	assert_zd_eq(future, future_ref, "input was touched");		\
} while (0)

	ASSERT_SHOULD_FAIL("alloc_max", NULL, NULL, NULL, 0);
	ASSERT_SHOULD_FAIL("alloc_max", &past, &len, NULL, 0);
	ASSERT_SHOULD_FAIL("alloc_max", NULL, NULL, &future, len);
	ASSERT_SHOULD_FAIL("alloc_max", &past, &len, &future, len);

#undef ASSERT_SHOULD_FAIL
}
TEST_END

TEST_BEGIN(test_prof_recent_on) {
	test_skip_if(!config_prof);

	ssize_t past, future;
	size_t len = sizeof(ssize_t);

	tsd_t *tsd = tsd_fetch();

	confirm_prof_setup(tsd);

	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, NULL, 0), 0, "no-op mallctl should be allowed");
	confirm_prof_setup(tsd);

	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, NULL, 0), 0, "Read error");
	assert_zd_eq(past, OPT_ALLOC_MAX, "Wrong read result");
	future = OPT_ALLOC_MAX + 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, len), 0, "Write error");
	future = -1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len), 0, "Read/write error");
	assert_zd_eq(past, OPT_ALLOC_MAX + 1, "Wrong read result");
	future = -2;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len), EINVAL,
	    "Invalid write should return EINVAL");
	assert_zd_eq(past, OPT_ALLOC_MAX + 1,
	    "Output should not be touched given invalid write");
	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len), 0, "Read/write error");
	assert_zd_eq(past, -1, "Wrong read result");
	future = OPT_ALLOC_MAX + 2;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len * 2), EINVAL,
	    "Invalid write should return EINVAL");
	assert_zd_eq(past, -1,
	    "Output should not be touched given invalid write");

	confirm_prof_setup(tsd);
}
TEST_END

/* Reproducible sequence of request sizes */
#define NTH_REQ_SIZE(n) ((n) * 97 + 101)

static void confirm_malloc(tsd_t *tsd, void *p) {
	assert_ptr_not_null(p, "malloc failed unexpectedly");
	edata_t *e = iealloc(TSDN_NULL, p);
	assert_ptr_not_null(e, "NULL edata for living pointer");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_t *n = edata_prof_recent_alloc_get(tsd, e);
	assert_ptr_not_null(n, "Record in edata should not be NULL");
	assert_ptr_not_null(n->alloc_tctx,
	    "alloc_tctx in record should not be NULL");
	assert_ptr_eq(e, n->alloc_edata,
	    "edata pointer in record is not correct");
	assert_ptr_null(n->dalloc_tctx, "dalloc_tctx in record should be NULL");
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
}

static void confirm_record_size(tsd_t *tsd, prof_recent_t *n, unsigned kth) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_zu_eq(n->size, NTH_REQ_SIZE(kth),
	    "Recorded allocation size is wrong");
}

static void confirm_record_living(tsd_t *tsd, prof_recent_t *n) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_ptr_not_null(n->alloc_tctx,
	    "alloc_tctx in record should not be NULL");
	assert_ptr_not_null(n->alloc_edata,
	    "Recorded edata should not be NULL for living pointer");
	assert_ptr_eq(n, edata_prof_recent_alloc_get(tsd, n->alloc_edata),
	    "Record in edata is not correct");
	assert_ptr_null(n->dalloc_tctx, "dalloc_tctx in record should be NULL");
}

static void confirm_record_released(tsd_t *tsd, prof_recent_t *n) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_ptr_not_null(n->alloc_tctx,
	    "alloc_tctx in record should not be NULL");
	assert_ptr_null(n->alloc_edata,
	    "Recorded edata should be NULL for released pointer");
	assert_ptr_not_null(n->dalloc_tctx,
	    "dalloc_tctx in record should not be NULL for released pointer");
}

TEST_BEGIN(test_prof_recent_alloc) {
	test_skip_if(!config_prof);

	bool b;
	unsigned i, c;
	size_t req_size;
	void *p;
	prof_recent_t *n;
	ssize_t future;

	tsd_t *tsd = tsd_fetch();

	confirm_prof_setup(tsd);

	/*
	 * First batch of 2 * OPT_ALLOC_MAX allocations.  After the
	 * (OPT_ALLOC_MAX - 1)'th allocation the recorded allocations should
	 * always be the last OPT_ALLOC_MAX allocations coming from here.
	 */
	for (i = 0; i < 2 * OPT_ALLOC_MAX; ++i) {
		req_size = NTH_REQ_SIZE(i);
		p = malloc(req_size);
		confirm_malloc(tsd, p);
		if (i < OPT_ALLOC_MAX - 1) {
			malloc_mutex_lock(tsd_tsdn(tsd),
			    &prof_recent_alloc_mtx);
			assert_ptr_ne(prof_recent_alloc_begin(tsd),
			    prof_recent_alloc_end(tsd),
			    "Empty recent allocation");
			malloc_mutex_unlock(tsd_tsdn(tsd),
			    &prof_recent_alloc_mtx);
			free(p);
			/*
			 * The recorded allocations may still include some
			 * other allocations before the test run started,
			 * so keep allocating without checking anything.
			 */
			continue;
		}
		c = 0;
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		for (n = prof_recent_alloc_begin(tsd);
		    n != prof_recent_alloc_end(tsd);
		    n = prof_recent_alloc_next(tsd, n)) {
			++c;
			confirm_record_size(tsd, n, i + c - OPT_ALLOC_MAX);
			if (c == OPT_ALLOC_MAX) {
				confirm_record_living(tsd, n);
			} else {
				confirm_record_released(tsd, n);
			}
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		assert_u_eq(c, OPT_ALLOC_MAX,
		    "Incorrect total number of allocations");
		free(p);
	}

	confirm_prof_setup(tsd);

	b = false;
	assert_d_eq(mallctl("prof.active", NULL, NULL, &b, sizeof(bool)), 0,
	    "mallctl for turning off prof_active failed");

	/*
	 * Second batch of OPT_ALLOC_MAX allocations.  Since prof_active is
	 * turned off, this batch shouldn't be recorded.
	 */
	for (; i < 3 * OPT_ALLOC_MAX; ++i) {
		req_size = NTH_REQ_SIZE(i);
		p = malloc(req_size);
		assert_ptr_not_null(p, "malloc failed unexpectedly");
		c = 0;
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		for (n = prof_recent_alloc_begin(tsd);
		    n != prof_recent_alloc_end(tsd);
		    n = prof_recent_alloc_next(tsd, n)) {
			confirm_record_size(tsd, n, c + OPT_ALLOC_MAX);
			confirm_record_released(tsd, n);
			++c;
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		assert_u_eq(c, OPT_ALLOC_MAX,
		    "Incorrect total number of allocations");
		free(p);
	}

	b = true;
	assert_d_eq(mallctl("prof.active", NULL, NULL, &b, sizeof(bool)), 0,
	    "mallctl for turning on prof_active failed");

	confirm_prof_setup(tsd);

	/*
	 * Third batch of OPT_ALLOC_MAX allocations.  Since prof_active is
	 * turned back on, they should be recorded, and in the list of recorded
	 * allocations they should follow the first batch rather than the
	 * second batch.
	 */
	for (; i < 4 * OPT_ALLOC_MAX; ++i) {
		req_size = NTH_REQ_SIZE(i);
		p = malloc(req_size);
		confirm_malloc(tsd, p);
		c = 0;
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		for (n = prof_recent_alloc_begin(tsd);
		    n != prof_recent_alloc_end(tsd);
		    n = prof_recent_alloc_next(tsd, n)) {
			++c;
			confirm_record_size(tsd, n,
			    /* Is the allocation from the third batch? */
			    i + c - OPT_ALLOC_MAX >= 3 * OPT_ALLOC_MAX ?
			    /* If yes, then it's just recorded. */
			    i + c - OPT_ALLOC_MAX :
			    /*
			     * Otherwise, it should come from the first batch
			     * instead of the second batch.
			     */
			    i + c - 2 * OPT_ALLOC_MAX);
			if (c == OPT_ALLOC_MAX) {
				confirm_record_living(tsd, n);
			} else {
				confirm_record_released(tsd, n);
			}
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		assert_u_eq(c, OPT_ALLOC_MAX,
		    "Incorrect total number of allocations");
		free(p);
	}

	/* Increasing the limit shouldn't alter the list of records. */
	future = OPT_ALLOC_MAX + 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
		++c;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX,
	    "Incorrect total number of allocations");

	/*
	 * Decreasing the limit shouldn't alter the list of records as long as
	 * the new limit is still no less than the length of the list.
	 */
	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
		++c;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX,
	    "Incorrect total number of allocations");

	/*
	 * Decreasing the limit should shorten the list of records if the new
	 * limit is less than the length of the list.
	 */
	future = OPT_ALLOC_MAX - 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		++c;
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX - 1,
	    "Incorrect total number of allocations");

	/* Setting to unlimited shouldn't alter the list of records. */
	future = -1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		++c;
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX - 1,
	    "Incorrect total number of allocations");

	/* Downshift to only one record. */
	future = 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	n = prof_recent_alloc_begin(tsd);
	assert(n != prof_recent_alloc_end(tsd));
	confirm_record_size(tsd, n, 4 * OPT_ALLOC_MAX - 1);
	confirm_record_released(tsd, n);
	n = prof_recent_alloc_next(tsd, n);
	assert(n == prof_recent_alloc_end(tsd));
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	/* Completely turn off. */
	future = 0;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert(prof_recent_alloc_begin(tsd) == prof_recent_alloc_end(tsd));
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	/* Restore the settings. */
	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert(prof_recent_alloc_begin(tsd) == prof_recent_alloc_end(tsd));
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	confirm_prof_setup(tsd);
}
TEST_END

#undef NTH_REQ_SIZE

int
main(void) {
	return test(
	    test_confirm_setup,
	    test_prof_recent_off,
	    test_prof_recent_on,
	    test_prof_recent_alloc);
}
