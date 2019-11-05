#define JEMALLOC_PROF_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ckh.h"
#include "jemalloc/internal/hash.h"
#include "jemalloc/internal/malloc_io.h"

/*
 * This file defines and manages the core profiling data structures.
 *
 * Conceptually, profiling data can be imagined as a table with three columns:
 * thread, stack trace, and current allocation size.  (When prof_accum is on,
 * there's one additional column which is the cumulative allocation size.)
 *
 * Implementation wise, each thread maintains a hash recording the stack trace
 * to allocation size correspondences, which are basically the individual rows
 * in the table.  In addition, two global "indices" are built to make data
 * aggregation efficient (for dumping): bt2gctx and tdatas, which are basically
 * the "grouped by stack trace" and "grouped by thread" views of the same table,
 * respectively.  Note that the allocation size is only aggregated to the two
 * indices at dumping time, so as to optimize for performance.
 */

/******************************************************************************/

/*
 * Global hash of (prof_bt_t *)-->(prof_gctx_t *).  This is the master data
 * structure that knows about all backtraces currently captured.
 */
static ckh_t		bt2gctx;

/*
 * Tree of all extant prof_tdata_t structures, regardless of state,
 * {attached,detached,expired}.
 */
static prof_tdata_tree_t	tdatas;

/*
 * This buffer is rather large for stack allocation, so use a single buffer for
 * all profile dumps.
 */
static char		prof_dump_buf[
    /* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
    PROF_DUMP_BUFSIZE
#else
    1
#endif
];
static size_t		prof_dump_buf_end;
static int		prof_dump_fd;

/******************************************************************************/
/* Red-black trees. */

static int
prof_tctx_comp(const prof_tctx_t *a, const prof_tctx_t *b) {
	uint64_t a_thr_uid = a->thr_uid;
	uint64_t b_thr_uid = b->thr_uid;
	int ret = (a_thr_uid > b_thr_uid) - (a_thr_uid < b_thr_uid);
	if (ret == 0) {
		uint64_t a_thr_discrim = a->thr_discrim;
		uint64_t b_thr_discrim = b->thr_discrim;
		ret = (a_thr_discrim > b_thr_discrim) - (a_thr_discrim <
		    b_thr_discrim);
		if (ret == 0) {
			uint64_t a_tctx_uid = a->tctx_uid;
			uint64_t b_tctx_uid = b->tctx_uid;
			ret = (a_tctx_uid > b_tctx_uid) - (a_tctx_uid <
			    b_tctx_uid);
		}
	}
	return ret;
}

rb_gen(static UNUSED, tctx_tree_, prof_tctx_tree_t, prof_tctx_t,
    tctx_link, prof_tctx_comp)

static int
prof_gctx_comp(const prof_gctx_t *a, const prof_gctx_t *b) {
	unsigned a_len = a->bt.len;
	unsigned b_len = b->bt.len;
	unsigned comp_len = (a_len < b_len) ? a_len : b_len;
	int ret = memcmp(a->bt.vec, b->bt.vec, comp_len * sizeof(void *));
	if (ret == 0) {
		ret = (a_len > b_len) - (a_len < b_len);
	}
	return ret;
}

rb_gen(static UNUSED, gctx_tree_, prof_gctx_tree_t, prof_gctx_t, dump_link,
    prof_gctx_comp)

static int
prof_tdata_comp(const prof_tdata_t *a, const prof_tdata_t *b) {
	int ret;
	uint64_t a_uid = a->thr_uid;
	uint64_t b_uid = b->thr_uid;

	ret = ((a_uid > b_uid) - (a_uid < b_uid));
	if (ret == 0) {
		uint64_t a_discrim = a->thr_discrim;
		uint64_t b_discrim = b->thr_discrim;

		ret = ((a_discrim > b_discrim) - (a_discrim < b_discrim));
	}
	return ret;
}

rb_gen(static UNUSED, tdata_tree_, prof_tdata_tree_t, prof_tdata_t, tdata_link,
    prof_tdata_comp)

/******************************************************************************/

bool
prof_data_init(tsd_t *tsd) {
	tdata_tree_new(&tdatas);
	return ckh_new(tsd, &bt2gctx, PROF_CKH_MINITEMS,
	    prof_bt_hash, prof_bt_keycomp);
}

static void
prof_enter(tsd_t *tsd, prof_tdata_t *tdata) {
	cassert(config_prof);
	assert(tdata == prof_tdata_get(tsd, false));

	if (tdata != NULL) {
		assert(!tdata->enq);
		tdata->enq = true;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &bt2gctx_mtx);
}

static void
prof_leave(tsd_t *tsd, prof_tdata_t *tdata) {
	cassert(config_prof);
	assert(tdata == prof_tdata_get(tsd, false));

	malloc_mutex_unlock(tsd_tsdn(tsd), &bt2gctx_mtx);

	if (tdata != NULL) {
		bool idump, gdump;

		assert(tdata->enq);
		tdata->enq = false;
		idump = tdata->enq_idump;
		tdata->enq_idump = false;
		gdump = tdata->enq_gdump;
		tdata->enq_gdump = false;

		if (idump) {
			prof_idump(tsd_tsdn(tsd));
		}
		if (gdump) {
			prof_gdump(tsd_tsdn(tsd));
		}
	}
}

static prof_gctx_t *
prof_gctx_create(tsdn_t *tsdn, prof_bt_t *bt) {
	/*
	 * Create a single allocation that has space for vec of length bt->len.
	 */
	size_t size = offsetof(prof_gctx_t, vec) + (bt->len * sizeof(void *));
	prof_gctx_t *gctx = (prof_gctx_t *)iallocztm(tsdn, size,
	    sz_size2index(size), false, NULL, true, arena_get(TSDN_NULL, 0, true),
	    true);
	if (gctx == NULL) {
		return NULL;
	}
	gctx->lock = prof_gctx_mutex_choose();
	/*
	 * Set nlimbo to 1, in order to avoid a race condition with
	 * prof_tctx_destroy()/prof_gctx_try_destroy().
	 */
	gctx->nlimbo = 1;
	tctx_tree_new(&gctx->tctxs);
	/* Duplicate bt. */
	memcpy(gctx->vec, bt->vec, bt->len * sizeof(void *));
	gctx->bt.vec = gctx->vec;
	gctx->bt.len = bt->len;
	return gctx;
}

static void
prof_gctx_try_destroy(tsd_t *tsd, prof_tdata_t *tdata_self, prof_gctx_t *gctx,
    prof_tdata_t *tdata) {
	cassert(config_prof);

	/*
	 * Check that gctx is still unused by any thread cache before destroying
	 * it.  prof_lookup() increments gctx->nlimbo in order to avoid a race
	 * condition with this function, as does prof_tctx_destroy() in order to
	 * avoid a race between the main body of prof_tctx_destroy() and entry
	 * into this function.
	 */
	prof_enter(tsd, tdata_self);
	malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
	assert(gctx->nlimbo != 0);
	if (tctx_tree_empty(&gctx->tctxs) && gctx->nlimbo == 1) {
		/* Remove gctx from bt2gctx. */
		if (ckh_remove(tsd, &bt2gctx, &gctx->bt, NULL, NULL)) {
			not_reached();
		}
		prof_leave(tsd, tdata_self);
		/* Destroy gctx. */
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
		idalloctm(tsd_tsdn(tsd), gctx, NULL, NULL, true, true);
	} else {
		/*
		 * Compensate for increment in prof_tctx_destroy() or
		 * prof_lookup().
		 */
		gctx->nlimbo--;
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
		prof_leave(tsd, tdata_self);
	}
}

static bool
prof_gctx_should_destroy(prof_gctx_t *gctx) {
	if (opt_prof_accum) {
		return false;
	}
	if (!tctx_tree_empty(&gctx->tctxs)) {
		return false;
	}
	if (gctx->nlimbo != 0) {
		return false;
	}
	return true;
}

static bool
prof_lookup_global(tsd_t *tsd, prof_bt_t *bt, prof_tdata_t *tdata,
    void **p_btkey, prof_gctx_t **p_gctx, bool *p_new_gctx) {
	union {
		prof_gctx_t	*p;
		void		*v;
	} gctx, tgctx;
	union {
		prof_bt_t	*p;
		void		*v;
	} btkey;
	bool new_gctx;

	prof_enter(tsd, tdata);
	if (ckh_search(&bt2gctx, bt, &btkey.v, &gctx.v)) {
		/* bt has never been seen before.  Insert it. */
		prof_leave(tsd, tdata);
		tgctx.p = prof_gctx_create(tsd_tsdn(tsd), bt);
		if (tgctx.v == NULL) {
			return true;
		}
		prof_enter(tsd, tdata);
		if (ckh_search(&bt2gctx, bt, &btkey.v, &gctx.v)) {
			gctx.p = tgctx.p;
			btkey.p = &gctx.p->bt;
			if (ckh_insert(tsd, &bt2gctx, btkey.v, gctx.v)) {
				/* OOM. */
				prof_leave(tsd, tdata);
				idalloctm(tsd_tsdn(tsd), gctx.v, NULL, NULL,
				    true, true);
				return true;
			}
			new_gctx = true;
		} else {
			new_gctx = false;
		}
	} else {
		tgctx.v = NULL;
		new_gctx = false;
	}

	if (!new_gctx) {
		/*
		 * Increment nlimbo, in order to avoid a race condition with
		 * prof_tctx_destroy()/prof_gctx_try_destroy().
		 */
		malloc_mutex_lock(tsd_tsdn(tsd), gctx.p->lock);
		gctx.p->nlimbo++;
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx.p->lock);
		new_gctx = false;

		if (tgctx.v != NULL) {
			/* Lost race to insert. */
			idalloctm(tsd_tsdn(tsd), tgctx.v, NULL, NULL, true,
			    true);
		}
	}
	prof_leave(tsd, tdata);

	*p_btkey = btkey.v;
	*p_gctx = gctx.p;
	*p_new_gctx = new_gctx;
	return false;
}

prof_tctx_t *
prof_lookup(tsd_t *tsd, prof_bt_t *bt) {
	union {
		prof_tctx_t	*p;
		void		*v;
	} ret;
	prof_tdata_t *tdata;
	bool not_found;

	cassert(config_prof);

	tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		return NULL;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), tdata->lock);
	not_found = ckh_search(&tdata->bt2tctx, bt, NULL, &ret.v);
	if (!not_found) { /* Note double negative! */
		ret.p->prepared = true;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
	if (not_found) {
		void *btkey;
		prof_gctx_t *gctx;
		bool new_gctx, error;

		/*
		 * This thread's cache lacks bt.  Look for it in the global
		 * cache.
		 */
		if (prof_lookup_global(tsd, bt, tdata, &btkey, &gctx,
		    &new_gctx)) {
			return NULL;
		}

		/* Link a prof_tctx_t into gctx for this thread. */
		ret.v = iallocztm(tsd_tsdn(tsd), sizeof(prof_tctx_t),
		    sz_size2index(sizeof(prof_tctx_t)), false, NULL, true,
		    arena_ichoose(tsd, NULL), true);
		if (ret.p == NULL) {
			if (new_gctx) {
				prof_gctx_try_destroy(tsd, tdata, gctx, tdata);
			}
			return NULL;
		}
		ret.p->tdata = tdata;
		ret.p->thr_uid = tdata->thr_uid;
		ret.p->thr_discrim = tdata->thr_discrim;
		memset(&ret.p->cnts, 0, sizeof(prof_cnt_t));
		ret.p->gctx = gctx;
		ret.p->tctx_uid = tdata->tctx_uid_next++;
		ret.p->prepared = true;
		ret.p->state = prof_tctx_state_initializing;
		malloc_mutex_lock(tsd_tsdn(tsd), tdata->lock);
		error = ckh_insert(tsd, &tdata->bt2tctx, btkey, ret.v);
		malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
		if (error) {
			if (new_gctx) {
				prof_gctx_try_destroy(tsd, tdata, gctx, tdata);
			}
			idalloctm(tsd_tsdn(tsd), ret.v, NULL, NULL, true, true);
			return NULL;
		}
		malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
		ret.p->state = prof_tctx_state_nominal;
		tctx_tree_insert(&gctx->tctxs, ret.p);
		gctx->nlimbo--;
		malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
	}

	return ret.p;
}

#ifdef JEMALLOC_JET
static prof_tdata_t *
prof_tdata_count_iter(prof_tdata_tree_t *tdatas, prof_tdata_t *tdata,
    void *arg) {
	size_t *tdata_count = (size_t *)arg;

	(*tdata_count)++;

	return NULL;
}

size_t
prof_tdata_count(void) {
	size_t tdata_count = 0;
	tsdn_t *tsdn;

	tsdn = tsdn_fetch();
	malloc_mutex_lock(tsdn, &tdatas_mtx);
	tdata_tree_iter(&tdatas, NULL, prof_tdata_count_iter,
	    (void *)&tdata_count);
	malloc_mutex_unlock(tsdn, &tdatas_mtx);

	return tdata_count;
}

size_t
prof_bt_count(void) {
	size_t bt_count;
	tsd_t *tsd;
	prof_tdata_t *tdata;

	tsd = tsd_fetch();
	tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		return 0;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &bt2gctx_mtx);
	bt_count = ckh_count(&bt2gctx);
	malloc_mutex_unlock(tsd_tsdn(tsd), &bt2gctx_mtx);

	return bt_count;
}
#endif

static int
prof_dump_open_impl(bool propagate_err, const char *filename) {
	int fd;

	fd = creat(filename, 0644);
	if (fd == -1 && !propagate_err) {
		malloc_printf("<jemalloc>: creat(\"%s\"), 0644) failed\n",
		    filename);
		if (opt_abort) {
			abort();
		}
	}

	return fd;
}
prof_dump_open_t *JET_MUTABLE prof_dump_open = prof_dump_open_impl;

static bool
prof_dump_flush(bool propagate_err) {
	bool ret = false;
	ssize_t err;

	cassert(config_prof);

	err = malloc_write_fd(prof_dump_fd, prof_dump_buf, prof_dump_buf_end);
	if (err == -1) {
		if (!propagate_err) {
			malloc_write("<jemalloc>: write() failed during heap "
			    "profile flush\n");
			if (opt_abort) {
				abort();
			}
		}
		ret = true;
	}
	prof_dump_buf_end = 0;

	return ret;
}

static bool
prof_dump_close(bool propagate_err) {
	bool ret;

	assert(prof_dump_fd != -1);
	ret = prof_dump_flush(propagate_err);
	close(prof_dump_fd);
	prof_dump_fd = -1;

	return ret;
}

static bool
prof_dump_write(bool propagate_err, const char *s) {
	size_t i, slen, n;

	cassert(config_prof);

	i = 0;
	slen = strlen(s);
	while (i < slen) {
		/* Flush the buffer if it is full. */
		if (prof_dump_buf_end == PROF_DUMP_BUFSIZE) {
			if (prof_dump_flush(propagate_err) && propagate_err) {
				return true;
			}
		}

		if (prof_dump_buf_end + slen - i <= PROF_DUMP_BUFSIZE) {
			/* Finish writing. */
			n = slen - i;
		} else {
			/* Write as much of s as will fit. */
			n = PROF_DUMP_BUFSIZE - prof_dump_buf_end;
		}
		memcpy(&prof_dump_buf[prof_dump_buf_end], &s[i], n);
		prof_dump_buf_end += n;
		i += n;
	}
	assert(i == slen);

	return false;
}

JEMALLOC_FORMAT_PRINTF(2, 3)
static bool
prof_dump_printf(bool propagate_err, const char *format, ...) {
	bool ret;
	va_list ap;
	char buf[PROF_PRINTF_BUFSIZE];

	va_start(ap, format);
	malloc_vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	ret = prof_dump_write(propagate_err, buf);

	return ret;
}

static void
prof_tctx_merge_tdata(tsdn_t *tsdn, prof_tctx_t *tctx, prof_tdata_t *tdata) {
	malloc_mutex_assert_owner(tsdn, tctx->tdata->lock);

	malloc_mutex_lock(tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_initializing:
		malloc_mutex_unlock(tsdn, tctx->gctx->lock);
		return;
	case prof_tctx_state_nominal:
		tctx->state = prof_tctx_state_dumping;
		malloc_mutex_unlock(tsdn, tctx->gctx->lock);

		memcpy(&tctx->dump_cnts, &tctx->cnts, sizeof(prof_cnt_t));

		tdata->cnt_summed.curobjs += tctx->dump_cnts.curobjs;
		tdata->cnt_summed.curbytes += tctx->dump_cnts.curbytes;
		if (opt_prof_accum) {
			tdata->cnt_summed.accumobjs +=
			    tctx->dump_cnts.accumobjs;
			tdata->cnt_summed.accumbytes +=
			    tctx->dump_cnts.accumbytes;
		}
		break;
	case prof_tctx_state_dumping:
	case prof_tctx_state_purgatory:
		not_reached();
	}
}

static void
prof_tctx_merge_gctx(tsdn_t *tsdn, prof_tctx_t *tctx, prof_gctx_t *gctx) {
	malloc_mutex_assert_owner(tsdn, gctx->lock);

	gctx->cnt_summed.curobjs += tctx->dump_cnts.curobjs;
	gctx->cnt_summed.curbytes += tctx->dump_cnts.curbytes;
	if (opt_prof_accum) {
		gctx->cnt_summed.accumobjs += tctx->dump_cnts.accumobjs;
		gctx->cnt_summed.accumbytes += tctx->dump_cnts.accumbytes;
	}
}

static prof_tctx_t *
prof_tctx_merge_iter(prof_tctx_tree_t *tctxs, prof_tctx_t *tctx, void *arg) {
	tsdn_t *tsdn = (tsdn_t *)arg;

	malloc_mutex_assert_owner(tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_nominal:
		/* New since dumping started; ignore. */
		break;
	case prof_tctx_state_dumping:
	case prof_tctx_state_purgatory:
		prof_tctx_merge_gctx(tsdn, tctx, tctx->gctx);
		break;
	default:
		not_reached();
	}

	return NULL;
}

struct prof_tctx_dump_iter_arg_s {
	tsdn_t	*tsdn;
	bool	propagate_err;
};

static prof_tctx_t *
prof_tctx_dump_iter(prof_tctx_tree_t *tctxs, prof_tctx_t *tctx, void *opaque) {
	struct prof_tctx_dump_iter_arg_s *arg =
	    (struct prof_tctx_dump_iter_arg_s *)opaque;

	malloc_mutex_assert_owner(arg->tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_initializing:
	case prof_tctx_state_nominal:
		/* Not captured by this dump. */
		break;
	case prof_tctx_state_dumping:
	case prof_tctx_state_purgatory:
		if (prof_dump_printf(arg->propagate_err,
		    "  t%"FMTu64": %"FMTu64": %"FMTu64" [%"FMTu64": "
		    "%"FMTu64"]\n", tctx->thr_uid, tctx->dump_cnts.curobjs,
		    tctx->dump_cnts.curbytes, tctx->dump_cnts.accumobjs,
		    tctx->dump_cnts.accumbytes)) {
			return tctx;
		}
		break;
	default:
		not_reached();
	}
	return NULL;
}

static prof_tctx_t *
prof_tctx_finish_iter(prof_tctx_tree_t *tctxs, prof_tctx_t *tctx, void *arg) {
	tsdn_t *tsdn = (tsdn_t *)arg;
	prof_tctx_t *ret;

	malloc_mutex_assert_owner(tsdn, tctx->gctx->lock);

	switch (tctx->state) {
	case prof_tctx_state_nominal:
		/* New since dumping started; ignore. */
		break;
	case prof_tctx_state_dumping:
		tctx->state = prof_tctx_state_nominal;
		break;
	case prof_tctx_state_purgatory:
		ret = tctx;
		goto label_return;
	default:
		not_reached();
	}

	ret = NULL;
label_return:
	return ret;
}

static void
prof_dump_gctx_prep(tsdn_t *tsdn, prof_gctx_t *gctx, prof_gctx_tree_t *gctxs) {
	cassert(config_prof);

	malloc_mutex_lock(tsdn, gctx->lock);

	/*
	 * Increment nlimbo so that gctx won't go away before dump.
	 * Additionally, link gctx into the dump list so that it is included in
	 * prof_dump()'s second pass.
	 */
	gctx->nlimbo++;
	gctx_tree_insert(gctxs, gctx);

	memset(&gctx->cnt_summed, 0, sizeof(prof_cnt_t));

	malloc_mutex_unlock(tsdn, gctx->lock);
}

struct prof_gctx_merge_iter_arg_s {
	tsdn_t	*tsdn;
	size_t	leak_ngctx;
};

static prof_gctx_t *
prof_gctx_merge_iter(prof_gctx_tree_t *gctxs, prof_gctx_t *gctx, void *opaque) {
	struct prof_gctx_merge_iter_arg_s *arg =
	    (struct prof_gctx_merge_iter_arg_s *)opaque;

	malloc_mutex_lock(arg->tsdn, gctx->lock);
	tctx_tree_iter(&gctx->tctxs, NULL, prof_tctx_merge_iter,
	    (void *)arg->tsdn);
	if (gctx->cnt_summed.curobjs != 0) {
		arg->leak_ngctx++;
	}
	malloc_mutex_unlock(arg->tsdn, gctx->lock);

	return NULL;
}

static void
prof_gctx_finish(tsd_t *tsd, prof_gctx_tree_t *gctxs) {
	prof_tdata_t *tdata = prof_tdata_get(tsd, false);
	prof_gctx_t *gctx;

	/*
	 * Standard tree iteration won't work here, because as soon as we
	 * decrement gctx->nlimbo and unlock gctx, another thread can
	 * concurrently destroy it, which will corrupt the tree.  Therefore,
	 * tear down the tree one node at a time during iteration.
	 */
	while ((gctx = gctx_tree_first(gctxs)) != NULL) {
		gctx_tree_remove(gctxs, gctx);
		malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
		{
			prof_tctx_t *next;

			next = NULL;
			do {
				prof_tctx_t *to_destroy =
				    tctx_tree_iter(&gctx->tctxs, next,
				    prof_tctx_finish_iter,
				    (void *)tsd_tsdn(tsd));
				if (to_destroy != NULL) {
					next = tctx_tree_next(&gctx->tctxs,
					    to_destroy);
					tctx_tree_remove(&gctx->tctxs,
					    to_destroy);
					idalloctm(tsd_tsdn(tsd), to_destroy,
					    NULL, NULL, true, true);
				} else {
					next = NULL;
				}
			} while (next != NULL);
		}
		gctx->nlimbo--;
		if (prof_gctx_should_destroy(gctx)) {
			gctx->nlimbo++;
			malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
			prof_gctx_try_destroy(tsd, tdata, gctx, tdata);
		} else {
			malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
		}
	}
}

struct prof_tdata_merge_iter_arg_s {
	tsdn_t		*tsdn;
	prof_cnt_t	cnt_all;
};

static prof_tdata_t *
prof_tdata_merge_iter(prof_tdata_tree_t *tdatas, prof_tdata_t *tdata,
    void *opaque) {
	struct prof_tdata_merge_iter_arg_s *arg =
	    (struct prof_tdata_merge_iter_arg_s *)opaque;

	malloc_mutex_lock(arg->tsdn, tdata->lock);
	if (!tdata->expired) {
		size_t tabind;
		union {
			prof_tctx_t	*p;
			void		*v;
		} tctx;

		tdata->dumping = true;
		memset(&tdata->cnt_summed, 0, sizeof(prof_cnt_t));
		for (tabind = 0; !ckh_iter(&tdata->bt2tctx, &tabind, NULL,
		    &tctx.v);) {
			prof_tctx_merge_tdata(arg->tsdn, tctx.p, tdata);
		}

		arg->cnt_all.curobjs += tdata->cnt_summed.curobjs;
		arg->cnt_all.curbytes += tdata->cnt_summed.curbytes;
		if (opt_prof_accum) {
			arg->cnt_all.accumobjs += tdata->cnt_summed.accumobjs;
			arg->cnt_all.accumbytes += tdata->cnt_summed.accumbytes;
		}
	} else {
		tdata->dumping = false;
	}
	malloc_mutex_unlock(arg->tsdn, tdata->lock);

	return NULL;
}

static prof_tdata_t *
prof_tdata_dump_iter(prof_tdata_tree_t *tdatas, prof_tdata_t *tdata,
    void *arg) {
	bool propagate_err = *(bool *)arg;

	if (!tdata->dumping) {
		return NULL;
	}

	if (prof_dump_printf(propagate_err,
	    "  t%"FMTu64": %"FMTu64": %"FMTu64" [%"FMTu64": %"FMTu64"]%s%s\n",
	    tdata->thr_uid, tdata->cnt_summed.curobjs,
	    tdata->cnt_summed.curbytes, tdata->cnt_summed.accumobjs,
	    tdata->cnt_summed.accumbytes,
	    (tdata->thread_name != NULL) ? " " : "",
	    (tdata->thread_name != NULL) ? tdata->thread_name : "")) {
		return tdata;
	}
	return NULL;
}

static bool
prof_dump_header_impl(tsdn_t *tsdn, bool propagate_err,
    const prof_cnt_t *cnt_all) {
	bool ret;

	if (prof_dump_printf(propagate_err,
	    "heap_v2/%"FMTu64"\n"
	    "  t*: %"FMTu64": %"FMTu64" [%"FMTu64": %"FMTu64"]\n",
	    ((uint64_t)1U << lg_prof_sample), cnt_all->curobjs,
	    cnt_all->curbytes, cnt_all->accumobjs, cnt_all->accumbytes)) {
		return true;
	}

	malloc_mutex_lock(tsdn, &tdatas_mtx);
	ret = (tdata_tree_iter(&tdatas, NULL, prof_tdata_dump_iter,
	    (void *)&propagate_err) != NULL);
	malloc_mutex_unlock(tsdn, &tdatas_mtx);
	return ret;
}
prof_dump_header_t *JET_MUTABLE prof_dump_header = prof_dump_header_impl;

static bool
prof_dump_gctx(tsdn_t *tsdn, bool propagate_err, prof_gctx_t *gctx,
    const prof_bt_t *bt, prof_gctx_tree_t *gctxs) {
	bool ret;
	unsigned i;
	struct prof_tctx_dump_iter_arg_s prof_tctx_dump_iter_arg;

	cassert(config_prof);
	malloc_mutex_assert_owner(tsdn, gctx->lock);

	/* Avoid dumping such gctx's that have no useful data. */
	if ((!opt_prof_accum && gctx->cnt_summed.curobjs == 0) ||
	    (opt_prof_accum && gctx->cnt_summed.accumobjs == 0)) {
		assert(gctx->cnt_summed.curobjs == 0);
		assert(gctx->cnt_summed.curbytes == 0);
		assert(gctx->cnt_summed.accumobjs == 0);
		assert(gctx->cnt_summed.accumbytes == 0);
		ret = false;
		goto label_return;
	}

	if (prof_dump_printf(propagate_err, "@")) {
		ret = true;
		goto label_return;
	}
	for (i = 0; i < bt->len; i++) {
		if (prof_dump_printf(propagate_err, " %#"FMTxPTR,
		    (uintptr_t)bt->vec[i])) {
			ret = true;
			goto label_return;
		}
	}

	if (prof_dump_printf(propagate_err,
	    "\n"
	    "  t*: %"FMTu64": %"FMTu64" [%"FMTu64": %"FMTu64"]\n",
	    gctx->cnt_summed.curobjs, gctx->cnt_summed.curbytes,
	    gctx->cnt_summed.accumobjs, gctx->cnt_summed.accumbytes)) {
		ret = true;
		goto label_return;
	}

	prof_tctx_dump_iter_arg.tsdn = tsdn;
	prof_tctx_dump_iter_arg.propagate_err = propagate_err;
	if (tctx_tree_iter(&gctx->tctxs, NULL, prof_tctx_dump_iter,
	    (void *)&prof_tctx_dump_iter_arg) != NULL) {
		ret = true;
		goto label_return;
	}

	ret = false;
label_return:
	return ret;
}

#ifndef _WIN32
JEMALLOC_FORMAT_PRINTF(1, 2)
static int
prof_open_maps(const char *format, ...) {
	int mfd;
	va_list ap;
	char filename[PATH_MAX + 1];

	va_start(ap, format);
	malloc_vsnprintf(filename, sizeof(filename), format, ap);
	va_end(ap);

#if defined(O_CLOEXEC)
	mfd = open(filename, O_RDONLY | O_CLOEXEC);
#else
	mfd = open(filename, O_RDONLY);
	if (mfd != -1) {
		fcntl(mfd, F_SETFD, fcntl(mfd, F_GETFD) | FD_CLOEXEC);
	}
#endif

	return mfd;
}
#endif

static bool
prof_dump_maps(bool propagate_err) {
	bool ret;
	int mfd;

	cassert(config_prof);
#ifdef __FreeBSD__
	mfd = prof_open_maps("/proc/curproc/map");
#elif defined(_WIN32)
	mfd = -1; // Not implemented
#else
	{
		int pid = prof_getpid();

		mfd = prof_open_maps("/proc/%d/task/%d/maps", pid, pid);
		if (mfd == -1) {
			mfd = prof_open_maps("/proc/%d/maps", pid);
		}
	}
#endif
	if (mfd != -1) {
		ssize_t nread;

		if (prof_dump_write(propagate_err, "\nMAPPED_LIBRARIES:\n") &&
		    propagate_err) {
			ret = true;
			goto label_return;
		}
		nread = 0;
		do {
			prof_dump_buf_end += nread;
			if (prof_dump_buf_end == PROF_DUMP_BUFSIZE) {
				/* Make space in prof_dump_buf before read(). */
				if (prof_dump_flush(propagate_err) &&
				    propagate_err) {
					ret = true;
					goto label_return;
				}
			}
			nread = malloc_read_fd(mfd,
			    &prof_dump_buf[prof_dump_buf_end], PROF_DUMP_BUFSIZE
			    - prof_dump_buf_end);
		} while (nread > 0);
	} else {
		ret = true;
		goto label_return;
	}

	ret = false;
label_return:
	if (mfd != -1) {
		close(mfd);
	}
	return ret;
}

/*
 * See prof_sample_threshold_update() comment for why the body of this function
 * is conditionally compiled.
 */
static void
prof_leakcheck(const prof_cnt_t *cnt_all, size_t leak_ngctx,
    const char *filename) {
#ifdef JEMALLOC_PROF
	/*
	 * Scaling is equivalent AdjustSamples() in jeprof, but the result may
	 * differ slightly from what jeprof reports, because here we scale the
	 * summary values, whereas jeprof scales each context individually and
	 * reports the sums of the scaled values.
	 */
	if (cnt_all->curbytes != 0) {
		double sample_period = (double)((uint64_t)1 << lg_prof_sample);
		double ratio = (((double)cnt_all->curbytes) /
		    (double)cnt_all->curobjs) / sample_period;
		double scale_factor = 1.0 / (1.0 - exp(-ratio));
		uint64_t curbytes = (uint64_t)round(((double)cnt_all->curbytes)
		    * scale_factor);
		uint64_t curobjs = (uint64_t)round(((double)cnt_all->curobjs) *
		    scale_factor);

		malloc_printf("<jemalloc>: Leak approximation summary: ~%"FMTu64
		    " byte%s, ~%"FMTu64" object%s, >= %zu context%s\n",
		    curbytes, (curbytes != 1) ? "s" : "", curobjs, (curobjs !=
		    1) ? "s" : "", leak_ngctx, (leak_ngctx != 1) ? "s" : "");
		malloc_printf(
		    "<jemalloc>: Run jeprof on \"%s\" for leak detail\n",
		    filename);
	}
#endif
}

struct prof_gctx_dump_iter_arg_s {
	tsdn_t	*tsdn;
	bool	propagate_err;
};

static prof_gctx_t *
prof_gctx_dump_iter(prof_gctx_tree_t *gctxs, prof_gctx_t *gctx, void *opaque) {
	prof_gctx_t *ret;
	struct prof_gctx_dump_iter_arg_s *arg =
	    (struct prof_gctx_dump_iter_arg_s *)opaque;

	malloc_mutex_lock(arg->tsdn, gctx->lock);

	if (prof_dump_gctx(arg->tsdn, arg->propagate_err, gctx, &gctx->bt,
	    gctxs)) {
		ret = gctx;
		goto label_return;
	}

	ret = NULL;
label_return:
	malloc_mutex_unlock(arg->tsdn, gctx->lock);
	return ret;
}

static void
prof_dump_prep(tsd_t *tsd, prof_tdata_t *tdata,
    struct prof_tdata_merge_iter_arg_s *prof_tdata_merge_iter_arg,
    struct prof_gctx_merge_iter_arg_s *prof_gctx_merge_iter_arg,
    prof_gctx_tree_t *gctxs) {
	size_t tabind;
	union {
		prof_gctx_t	*p;
		void		*v;
	} gctx;

	prof_enter(tsd, tdata);

	/*
	 * Put gctx's in limbo and clear their counters in preparation for
	 * summing.
	 */
	gctx_tree_new(gctxs);
	for (tabind = 0; !ckh_iter(&bt2gctx, &tabind, NULL, &gctx.v);) {
		prof_dump_gctx_prep(tsd_tsdn(tsd), gctx.p, gctxs);
	}

	/*
	 * Iterate over tdatas, and for the non-expired ones snapshot their tctx
	 * stats and merge them into the associated gctx's.
	 */
	prof_tdata_merge_iter_arg->tsdn = tsd_tsdn(tsd);
	memset(&prof_tdata_merge_iter_arg->cnt_all, 0, sizeof(prof_cnt_t));
	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);
	tdata_tree_iter(&tdatas, NULL, prof_tdata_merge_iter,
	    (void *)prof_tdata_merge_iter_arg);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);

	/* Merge tctx stats into gctx's. */
	prof_gctx_merge_iter_arg->tsdn = tsd_tsdn(tsd);
	prof_gctx_merge_iter_arg->leak_ngctx = 0;
	gctx_tree_iter(gctxs, NULL, prof_gctx_merge_iter,
	    (void *)prof_gctx_merge_iter_arg);

	prof_leave(tsd, tdata);
}

static bool
prof_dump_file(tsd_t *tsd, bool propagate_err, const char *filename,
    bool leakcheck, prof_tdata_t *tdata,
    struct prof_tdata_merge_iter_arg_s *prof_tdata_merge_iter_arg,
    struct prof_gctx_merge_iter_arg_s *prof_gctx_merge_iter_arg,
    struct prof_gctx_dump_iter_arg_s *prof_gctx_dump_iter_arg,
    prof_gctx_tree_t *gctxs) {
	/* Create dump file. */
	if ((prof_dump_fd = prof_dump_open(propagate_err, filename)) == -1) {
		return true;
	}

	/* Dump profile header. */
	if (prof_dump_header(tsd_tsdn(tsd), propagate_err,
	    &prof_tdata_merge_iter_arg->cnt_all)) {
		goto label_write_error;
	}

	/* Dump per gctx profile stats. */
	prof_gctx_dump_iter_arg->tsdn = tsd_tsdn(tsd);
	prof_gctx_dump_iter_arg->propagate_err = propagate_err;
	if (gctx_tree_iter(gctxs, NULL, prof_gctx_dump_iter,
	    (void *)prof_gctx_dump_iter_arg) != NULL) {
		goto label_write_error;
	}

	/* Dump /proc/<pid>/maps if possible. */
	if (prof_dump_maps(propagate_err)) {
		goto label_write_error;
	}

	if (prof_dump_close(propagate_err)) {
		return true;
	}

	return false;
label_write_error:
	prof_dump_close(propagate_err);
	return true;
}

bool
prof_dump(tsd_t *tsd, bool propagate_err, const char *filename,
    bool leakcheck) {
	cassert(config_prof);
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t * tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return true;
	}

	pre_reentrancy(tsd, NULL);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_mtx);

	prof_gctx_tree_t gctxs;
	struct prof_tdata_merge_iter_arg_s prof_tdata_merge_iter_arg;
	struct prof_gctx_merge_iter_arg_s prof_gctx_merge_iter_arg;
	struct prof_gctx_dump_iter_arg_s prof_gctx_dump_iter_arg;
	prof_dump_prep(tsd, tdata, &prof_tdata_merge_iter_arg,
	    &prof_gctx_merge_iter_arg, &gctxs);
	bool err = prof_dump_file(tsd, propagate_err, filename, leakcheck, tdata,
	    &prof_tdata_merge_iter_arg, &prof_gctx_merge_iter_arg,
	    &prof_gctx_dump_iter_arg, &gctxs);
	prof_gctx_finish(tsd, &gctxs);

	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_mtx);
	post_reentrancy(tsd);

	if (err) {
		return true;
	}

	if (leakcheck) {
		prof_leakcheck(&prof_tdata_merge_iter_arg.cnt_all,
		    prof_gctx_merge_iter_arg.leak_ngctx, filename);
	}
	return false;
}

#ifdef JEMALLOC_JET
void
prof_cnt_all(uint64_t *curobjs, uint64_t *curbytes, uint64_t *accumobjs,
    uint64_t *accumbytes) {
	tsd_t *tsd;
	prof_tdata_t *tdata;
	struct prof_tdata_merge_iter_arg_s prof_tdata_merge_iter_arg;
	struct prof_gctx_merge_iter_arg_s prof_gctx_merge_iter_arg;
	prof_gctx_tree_t gctxs;

	tsd = tsd_fetch();
	tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		if (curobjs != NULL) {
			*curobjs = 0;
		}
		if (curbytes != NULL) {
			*curbytes = 0;
		}
		if (accumobjs != NULL) {
			*accumobjs = 0;
		}
		if (accumbytes != NULL) {
			*accumbytes = 0;
		}
		return;
	}

	prof_dump_prep(tsd, tdata, &prof_tdata_merge_iter_arg,
	    &prof_gctx_merge_iter_arg, &gctxs);
	prof_gctx_finish(tsd, &gctxs);

	if (curobjs != NULL) {
		*curobjs = prof_tdata_merge_iter_arg.cnt_all.curobjs;
	}
	if (curbytes != NULL) {
		*curbytes = prof_tdata_merge_iter_arg.cnt_all.curbytes;
	}
	if (accumobjs != NULL) {
		*accumobjs = prof_tdata_merge_iter_arg.cnt_all.accumobjs;
	}
	if (accumbytes != NULL) {
		*accumbytes = prof_tdata_merge_iter_arg.cnt_all.accumbytes;
	}
}
#endif

void
prof_bt_hash(const void *key, size_t r_hash[2]) {
	prof_bt_t *bt = (prof_bt_t *)key;

	cassert(config_prof);

	hash(bt->vec, bt->len * sizeof(void *), 0x94122f33U, r_hash);
}

bool
prof_bt_keycomp(const void *k1, const void *k2) {
	const prof_bt_t *bt1 = (prof_bt_t *)k1;
	const prof_bt_t *bt2 = (prof_bt_t *)k2;

	cassert(config_prof);

	if (bt1->len != bt2->len) {
		return false;
	}
	return (memcmp(bt1->vec, bt2->vec, bt1->len * sizeof(void *)) == 0);
}

prof_tdata_t *
prof_tdata_init_impl(tsd_t *tsd, uint64_t thr_uid, uint64_t thr_discrim,
    char *thread_name, bool active, bool reset_interval) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	cassert(config_prof);

	/* Initialize an empty cache for this thread. */
	tdata = (prof_tdata_t *)iallocztm(tsd_tsdn(tsd), sizeof(prof_tdata_t),
	    sz_size2index(sizeof(prof_tdata_t)), false, NULL, true,
	    arena_get(TSDN_NULL, 0, true), true);
	if (tdata == NULL) {
		return NULL;
	}

	tdata->lock = prof_tdata_mutex_choose(thr_uid);
	tdata->thr_uid = thr_uid;
	tdata->thr_discrim = thr_discrim;
	tdata->thread_name = thread_name;
	tdata->attached = true;
	tdata->expired = false;
	tdata->tctx_uid_next = 0;

	if (ckh_new(tsd, &tdata->bt2tctx, PROF_CKH_MINITEMS, prof_bt_hash,
	    prof_bt_keycomp)) {
		idalloctm(tsd_tsdn(tsd), tdata, NULL, NULL, true, true);
		return NULL;
	}

	if (reset_interval) {
		prof_sample_threshold_update(tsd);
	}

	tdata->enq = false;
	tdata->enq_idump = false;
	tdata->enq_gdump = false;

	tdata->dumping = false;
	tdata->active = active;

	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);
	tdata_tree_insert(&tdatas, tdata);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);

	return tdata;
}

static bool
prof_tdata_should_destroy_unlocked(prof_tdata_t *tdata, bool even_if_attached) {
	if (tdata->attached && !even_if_attached) {
		return false;
	}
	if (ckh_count(&tdata->bt2tctx) != 0) {
		return false;
	}
	return true;
}

static bool
prof_tdata_should_destroy(tsdn_t *tsdn, prof_tdata_t *tdata,
    bool even_if_attached) {
	malloc_mutex_assert_owner(tsdn, tdata->lock);

	return prof_tdata_should_destroy_unlocked(tdata, even_if_attached);
}

static void
prof_tdata_destroy_locked(tsd_t *tsd, prof_tdata_t *tdata,
    bool even_if_attached) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tdatas_mtx);

	tdata_tree_remove(&tdatas, tdata);

	assert(prof_tdata_should_destroy_unlocked(tdata, even_if_attached));

	if (tdata->thread_name != NULL) {
		idalloctm(tsd_tsdn(tsd), tdata->thread_name, NULL, NULL, true,
		    true);
	}
	ckh_delete(tsd, &tdata->bt2tctx);
	idalloctm(tsd_tsdn(tsd), tdata, NULL, NULL, true, true);
}

static void
prof_tdata_destroy(tsd_t *tsd, prof_tdata_t *tdata, bool even_if_attached) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);
	prof_tdata_destroy_locked(tsd, tdata, even_if_attached);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);
}

void
prof_tdata_detach(tsd_t *tsd, prof_tdata_t *tdata) {
	bool destroy_tdata;

	malloc_mutex_lock(tsd_tsdn(tsd), tdata->lock);
	if (tdata->attached) {
		destroy_tdata = prof_tdata_should_destroy(tsd_tsdn(tsd), tdata,
		    true);
		/*
		 * Only detach if !destroy_tdata, because detaching would allow
		 * another thread to win the race to destroy tdata.
		 */
		if (!destroy_tdata) {
			tdata->attached = false;
		}
		tsd_prof_tdata_set(tsd, NULL);
	} else {
		destroy_tdata = false;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);
	if (destroy_tdata) {
		prof_tdata_destroy(tsd, tdata, true);
	}
}

static bool
prof_tdata_expire(tsdn_t *tsdn, prof_tdata_t *tdata) {
	bool destroy_tdata;

	malloc_mutex_lock(tsdn, tdata->lock);
	if (!tdata->expired) {
		tdata->expired = true;
		destroy_tdata = tdata->attached ? false :
		    prof_tdata_should_destroy(tsdn, tdata, false);
	} else {
		destroy_tdata = false;
	}
	malloc_mutex_unlock(tsdn, tdata->lock);

	return destroy_tdata;
}

static prof_tdata_t *
prof_tdata_reset_iter(prof_tdata_tree_t *tdatas, prof_tdata_t *tdata,
    void *arg) {
	tsdn_t *tsdn = (tsdn_t *)arg;

	return (prof_tdata_expire(tsdn, tdata) ? tdata : NULL);
}

void
prof_reset(tsd_t *tsd, size_t lg_sample) {
	prof_tdata_t *next;

	assert(lg_sample < (sizeof(uint64_t) << 3));

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_mtx);
	malloc_mutex_lock(tsd_tsdn(tsd), &tdatas_mtx);

	lg_prof_sample = lg_sample;

	next = NULL;
	do {
		prof_tdata_t *to_destroy = tdata_tree_iter(&tdatas, next,
		    prof_tdata_reset_iter, (void *)tsd);
		if (to_destroy != NULL) {
			next = tdata_tree_next(&tdatas, to_destroy);
			prof_tdata_destroy_locked(tsd, to_destroy, false);
		} else {
			next = NULL;
		}
	} while (next != NULL);

	malloc_mutex_unlock(tsd_tsdn(tsd), &tdatas_mtx);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_mtx);
}

void
prof_tctx_destroy(tsd_t *tsd, prof_tctx_t *tctx) {
	prof_tdata_t *tdata = tctx->tdata;
	prof_gctx_t *gctx = tctx->gctx;
	bool destroy_tdata, destroy_tctx, destroy_gctx;

	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);

	assert(tctx->cnts.curobjs == 0);
	assert(tctx->cnts.curbytes == 0);
	assert(!opt_prof_accum);
	assert(tctx->cnts.accumobjs == 0);
	assert(tctx->cnts.accumbytes == 0);

	ckh_remove(tsd, &tdata->bt2tctx, &gctx->bt, NULL, NULL);
	destroy_tdata = prof_tdata_should_destroy(tsd_tsdn(tsd), tdata, false);
	malloc_mutex_unlock(tsd_tsdn(tsd), tdata->lock);

	malloc_mutex_lock(tsd_tsdn(tsd), gctx->lock);
	switch (tctx->state) {
	case prof_tctx_state_nominal:
		tctx_tree_remove(&gctx->tctxs, tctx);
		destroy_tctx = true;
		if (prof_gctx_should_destroy(gctx)) {
			/*
			 * Increment gctx->nlimbo in order to keep another
			 * thread from winning the race to destroy gctx while
			 * this one has gctx->lock dropped.  Without this, it
			 * would be possible for another thread to:
			 *
			 * 1) Sample an allocation associated with gctx.
			 * 2) Deallocate the sampled object.
			 * 3) Successfully prof_gctx_try_destroy(gctx).
			 *
			 * The result would be that gctx no longer exists by the
			 * time this thread accesses it in
			 * prof_gctx_try_destroy().
			 */
			gctx->nlimbo++;
			destroy_gctx = true;
		} else {
			destroy_gctx = false;
		}
		break;
	case prof_tctx_state_dumping:
		/*
		 * A dumping thread needs tctx to remain valid until dumping
		 * has finished.  Change state such that the dumping thread will
		 * complete destruction during a late dump iteration phase.
		 */
		tctx->state = prof_tctx_state_purgatory;
		destroy_tctx = false;
		destroy_gctx = false;
		break;
	default:
		not_reached();
		destroy_tctx = false;
		destroy_gctx = false;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), gctx->lock);
	if (destroy_gctx) {
		prof_gctx_try_destroy(tsd, prof_tdata_get(tsd, false), gctx,
		    tdata);
	}

	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), tctx->tdata->lock);

	if (destroy_tdata) {
		prof_tdata_destroy(tsd, tdata, false);
	}

	if (destroy_tctx) {
		idalloctm(tsd_tsdn(tsd), tctx, NULL, NULL, true, true);
	}
}

/******************************************************************************/
