#define	JEMALLOC_PROF_C_
#include "jemalloc/internal/jemalloc_internal.h"
#ifdef JEMALLOC_PROF
/******************************************************************************/

#ifdef JEMALLOC_PROF_LIBGCC
#include <unwind.h>
#endif

#ifdef JEMALLOC_PROF_LIBUNWIND
#define	UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#include <math.h>

/******************************************************************************/
/* Data. */

bool		opt_prof = false;
bool		opt_prof_active = true;
size_t		opt_lg_prof_bt_max = LG_PROF_BT_MAX_DEFAULT;
size_t		opt_lg_prof_sample = LG_PROF_SAMPLE_DEFAULT;
ssize_t		opt_lg_prof_interval = LG_PROF_INTERVAL_DEFAULT;
bool		opt_prof_udump = false;
bool		opt_prof_leak = false;

uint64_t	prof_interval;
bool		prof_promote;

/*
 * Global hash of (prof_bt_t *)-->(prof_ctx_t *).  This is the master data
 * structure that knows about all backtraces ever captured.
 */
static ckh_t		bt2ctx;
static malloc_mutex_t	bt2ctx_mtx;

/*
 * Thread-specific hash of (prof_bt_t *)-->(prof_thr_cnt_t *).  Each thread
 * keeps a cache of backtraces, with associated thread-specific prof_thr_cnt_t
 * objects.  Other threads may read the prof_thr_cnt_t contents, but no others
 * will ever write them.
 *
 * Upon thread exit, the thread must merge all the prof_thr_cnt_t counter data
 * into the associated prof_ctx_t objects, and unlink/free the prof_thr_cnt_t
 * objects.
 */
static __thread ckh_t	*bt2cnt_tls JEMALLOC_ATTR(tls_model("initial-exec"));

/*
 * Same contents as b2cnt, but initialized such that the TSD destructor is
 * called when a thread exits, so that bt2cnt_tls contents can be merged,
 * unlinked, and deallocated.
 */
static pthread_key_t	bt2cnt_tsd;

/* (1U << opt_lg_prof_bt_max). */
static unsigned		prof_bt_max;

static __thread uint64_t prof_sample_prn_state
    JEMALLOC_ATTR(tls_model("initial-exec"));
static __thread uint64_t prof_sample_threshold
    JEMALLOC_ATTR(tls_model("initial-exec"));
static __thread uint64_t prof_sample_accum
    JEMALLOC_ATTR(tls_model("initial-exec"));

static malloc_mutex_t	prof_dump_seq_mtx;
static uint64_t		prof_dump_seq;
static uint64_t		prof_dump_iseq;
static uint64_t		prof_dump_mseq;
static uint64_t		prof_dump_useq;

/*
 * This buffer is rather large for stack allocation, so use a single buffer for
 * all profile dumps.  The buffer is implicitly protected by bt2ctx_mtx, since
 * it must be locked anyway during dumping.
 */
static char		prof_dump_buf[PROF_DUMP_BUF_SIZE];
static unsigned		prof_dump_buf_end;
static int		prof_dump_fd;

/* Do not dump any profiles until bootstrapping is complete. */
static bool		prof_booted = false;

static malloc_mutex_t	enq_mtx;
static bool		enq;
static bool		enq_idump;
static bool		enq_udump;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static prof_bt_t	*bt_dup(prof_bt_t *bt);
static void	bt_init(prof_bt_t *bt, void **vec);
#ifdef JEMALLOC_PROF_LIBGCC
static _Unwind_Reason_Code	prof_unwind_init_callback(
    struct _Unwind_Context *context, void *arg);
static _Unwind_Reason_Code	prof_unwind_callback(
    struct _Unwind_Context *context, void *arg);
#endif
static void	prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max);
static prof_thr_cnt_t	*prof_lookup(prof_bt_t *bt);
static void	prof_cnt_set(const void *ptr, prof_thr_cnt_t *cnt);
static bool	prof_flush(bool propagate_err);
static bool	prof_write(const char *s, bool propagate_err);
static void	prof_ctx_merge(prof_ctx_t *ctx, prof_cnt_t *cnt_all,
    size_t *leak_nctx);
static bool	prof_dump_ctx(prof_ctx_t *ctx, prof_bt_t *bt,
    bool propagate_err);
static bool	prof_dump_maps(bool propagate_err);
static bool	prof_dump(const char *filename, bool leakcheck,
    bool propagate_err);
static void	prof_dump_filename(char *filename, char v, int64_t vseq);
static void	prof_fdump(void);
static void	prof_bt_hash(const void *key, unsigned minbits, size_t *hash1,
    size_t *hash2);
static bool	prof_bt_keycomp(const void *k1, const void *k2);
static void	bt2cnt_thread_cleanup(void *arg);

/******************************************************************************/

static void
bt_init(prof_bt_t *bt, void **vec)
{

	bt->vec = vec;
	bt->len = 0;
}

static prof_bt_t *
bt_dup(prof_bt_t *bt)
{
	prof_bt_t *ret;

	/*
	 * Create a single allocation that has space for vec immediately
	 * following the prof_bt_t structure.  The backtraces that get
	 * stored in the backtrace caches are copied from stack-allocated
	 * temporary variables, so size is known at creation time.  Making this
	 * a contiguous object improves cache locality.
	 */
	ret = (prof_bt_t *)imalloc(QUANTUM_CEILING(sizeof(prof_bt_t)) +
	    (bt->len * sizeof(void *)));
	if (ret == NULL)
		return (NULL);
	ret->vec = (void **)((uintptr_t)ret +
	    QUANTUM_CEILING(sizeof(prof_bt_t)));
	memcpy(ret->vec, bt->vec, bt->len * sizeof(void *));
	ret->len = bt->len;

	return (ret);
}

static inline void
prof_enter(void)
{

	malloc_mutex_lock(&enq_mtx);
	enq = true;
	malloc_mutex_unlock(&enq_mtx);

	malloc_mutex_lock(&bt2ctx_mtx);
}

static inline void
prof_leave(void)
{
	bool idump, udump;

	malloc_mutex_unlock(&bt2ctx_mtx);

	malloc_mutex_lock(&enq_mtx);
	enq = false;
	idump = enq_idump;
	enq_idump = false;
	udump = enq_udump;
	enq_udump = false;
	malloc_mutex_unlock(&enq_mtx);

	if (idump)
		prof_idump();
	if (udump)
		prof_udump();
}

#ifdef JEMALLOC_PROF_LIBGCC
static _Unwind_Reason_Code
prof_unwind_init_callback(struct _Unwind_Context *context, void *arg)
{

	return (_URC_NO_REASON);
}

static _Unwind_Reason_Code
prof_unwind_callback(struct _Unwind_Context *context, void *arg)
{
	prof_unwind_data_t *data = (prof_unwind_data_t *)arg;

	if (data->nignore > 0)
		data->nignore--;
	else {
		data->bt->vec[data->bt->len] = (void *)_Unwind_GetIP(context);
		data->bt->len++;
		if (data->bt->len == data->max)
			return (_URC_END_OF_STACK);
	}

	return (_URC_NO_REASON);
}

static void
prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max)
{
	prof_unwind_data_t data = {bt, nignore, max};

	_Unwind_Backtrace(prof_unwind_callback, &data);
}
#elif defined(JEMALLOC_PROF_LIBUNWIND)
static void
prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max)
{
	unw_context_t uc;
	unw_cursor_t cursor;
	unsigned i;
	int err;

	assert(bt->len == 0);
	assert(bt->vec != NULL);
	assert(max <= (1U << opt_lg_prof_bt_max));

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);

	/* Throw away (nignore+1) stack frames, if that many exist. */
	for (i = 0; i < nignore + 1; i++) {
		err = unw_step(&cursor);
		if (err <= 0)
			return;
	}

	/*
	 * Iterate over stack frames until there are no more.  Heap-allocate
	 * and iteratively grow a larger bt if necessary.
	 */
	for (i = 0; i < max; i++) {
		unw_get_reg(&cursor, UNW_REG_IP, (unw_word_t *)&bt->vec[i]);
		err = unw_step(&cursor);
		if (err <= 0) {
			bt->len = i;
			break;
		}
	}
}
#else
static void
prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max)
{
#define	NIGNORE	3
#define	BT_FRAME(i)							\
	if ((i) < NIGNORE + max) {					\
		void *p;						\
		if (__builtin_frame_address(i) == 0)			\
			return;						\
		p = __builtin_return_address(i);			\
		if (p == NULL)						\
			return;						\
		if (i >= NIGNORE) {					\
			bt->vec[(i) - NIGNORE] = p;			\
			bt->len = (i) - NIGNORE + 1;			\
		}							\
	} else								\
		return;

	assert(max <= (1U << opt_lg_prof_bt_max));

	/*
	 * Ignore the first three frames, since they are:
	 *
	 *   0: prof_backtrace()
	 *   1: prof_alloc_prep()
	 *   2: malloc(), calloc(), etc.
	 */
#if 1
	assert(nignore + 1 == NIGNORE);
#else
	BT_FRAME(0)
	BT_FRAME(1)
	BT_FRAME(2)
#endif
	BT_FRAME(3)
	BT_FRAME(4)
	BT_FRAME(5)
	BT_FRAME(6)
	BT_FRAME(7)
	BT_FRAME(8)
	BT_FRAME(9)

	BT_FRAME(10)
	BT_FRAME(11)
	BT_FRAME(12)
	BT_FRAME(13)
	BT_FRAME(14)
	BT_FRAME(15)
	BT_FRAME(16)
	BT_FRAME(17)
	BT_FRAME(18)
	BT_FRAME(19)

	BT_FRAME(20)
	BT_FRAME(21)
	BT_FRAME(22)
	BT_FRAME(23)
	BT_FRAME(24)
	BT_FRAME(25)
	BT_FRAME(26)
	BT_FRAME(27)
	BT_FRAME(28)
	BT_FRAME(29)

	BT_FRAME(30)
	BT_FRAME(31)
	BT_FRAME(32)
	BT_FRAME(33)
	BT_FRAME(34)
	BT_FRAME(35)
	BT_FRAME(36)
	BT_FRAME(37)
	BT_FRAME(38)
	BT_FRAME(39)

	BT_FRAME(40)
	BT_FRAME(41)
	BT_FRAME(42)
	BT_FRAME(43)
	BT_FRAME(44)
	BT_FRAME(45)
	BT_FRAME(46)
	BT_FRAME(47)
	BT_FRAME(48)
	BT_FRAME(49)

	BT_FRAME(50)
	BT_FRAME(51)
	BT_FRAME(52)
	BT_FRAME(53)
	BT_FRAME(54)
	BT_FRAME(55)
	BT_FRAME(56)
	BT_FRAME(57)
	BT_FRAME(58)
	BT_FRAME(59)

	BT_FRAME(60)
	BT_FRAME(61)
	BT_FRAME(62)
	BT_FRAME(63)
	BT_FRAME(64)
	BT_FRAME(65)
	BT_FRAME(66)
	BT_FRAME(67)
	BT_FRAME(68)
	BT_FRAME(69)

	BT_FRAME(70)
	BT_FRAME(71)
	BT_FRAME(72)
	BT_FRAME(73)
	BT_FRAME(74)
	BT_FRAME(75)
	BT_FRAME(76)
	BT_FRAME(77)
	BT_FRAME(78)
	BT_FRAME(79)

	BT_FRAME(80)
	BT_FRAME(81)
	BT_FRAME(82)
	BT_FRAME(83)
	BT_FRAME(84)
	BT_FRAME(85)
	BT_FRAME(86)
	BT_FRAME(87)
	BT_FRAME(88)
	BT_FRAME(89)

	BT_FRAME(90)
	BT_FRAME(91)
	BT_FRAME(92)
	BT_FRAME(93)
	BT_FRAME(94)
	BT_FRAME(95)
	BT_FRAME(96)
	BT_FRAME(97)
	BT_FRAME(98)
	BT_FRAME(99)

	BT_FRAME(100)
	BT_FRAME(101)
	BT_FRAME(102)
	BT_FRAME(103)
	BT_FRAME(104)
	BT_FRAME(105)
	BT_FRAME(106)
	BT_FRAME(107)
	BT_FRAME(108)
	BT_FRAME(109)

	BT_FRAME(110)
	BT_FRAME(111)
	BT_FRAME(112)
	BT_FRAME(113)
	BT_FRAME(114)
	BT_FRAME(115)
	BT_FRAME(116)
	BT_FRAME(117)
	BT_FRAME(118)
	BT_FRAME(119)

	BT_FRAME(120)
	BT_FRAME(121)
	BT_FRAME(122)
	BT_FRAME(123)
	BT_FRAME(124)
	BT_FRAME(125)
	BT_FRAME(126)
	BT_FRAME(127)

	/* Extras to compensate for NIGNORE. */
	BT_FRAME(128)
	BT_FRAME(129)
	BT_FRAME(130)
#undef BT_FRAME
}
#endif

static prof_thr_cnt_t *
prof_lookup(prof_bt_t *bt)
{
	prof_thr_cnt_t *ret;
	ckh_t *bt2cnt = bt2cnt_tls;

	if (bt2cnt == NULL) {
		/* Initialize an empty cache for this thread. */
		bt2cnt = (ckh_t *)imalloc(sizeof(ckh_t));
		if (bt2cnt == NULL)
			return (NULL);
		if (ckh_new(bt2cnt, PROF_CKH_MINITEMS, prof_bt_hash,
		    prof_bt_keycomp)) {
			idalloc(bt2cnt);
			return (NULL);
		}
		bt2cnt_tls = bt2cnt;
	}

	if (ckh_search(bt2cnt, bt, NULL, (void **)&ret)) {
		prof_bt_t *btkey;
		prof_ctx_t *ctx;

		/*
		 * This thread's cache lacks bt.  Look for it in the global
		 * cache.
		 */
		prof_enter();
		if (ckh_search(&bt2ctx, bt, (void **)&btkey, (void **)&ctx)) {

			/* bt has never been seen before.  Insert it. */
			ctx = (prof_ctx_t *)imalloc(sizeof(prof_ctx_t));
			if (ctx == NULL) {
				prof_leave();
				return (NULL);
			}
			btkey = bt_dup(bt);
			if (btkey == NULL) {
				prof_leave();
				idalloc(ctx);
				return (NULL);
			}
			if (malloc_mutex_init(&ctx->lock)) {
				prof_leave();
				idalloc(btkey);
				idalloc(ctx);
				return (NULL);
			}
			memset(&ctx->cnt_merged, 0, sizeof(prof_cnt_t));
			ql_new(&ctx->cnts_ql);
			if (ckh_insert(&bt2ctx, btkey, ctx)) {
				/* OOM. */
				prof_leave();
				idalloc(btkey);
				idalloc(ctx);
				return (NULL);
			}
		}
		prof_leave();

		/* Link a prof_thd_cnt_t into ctx for this thread. */
		ret = (prof_thr_cnt_t *)imalloc(sizeof(prof_thr_cnt_t));
		if (ret == NULL)
			return (NULL);
		ql_elm_new(ret, link);
		ret->ctx = ctx;
		ret->epoch = 0;
		memset(&ret->cnts, 0, sizeof(prof_cnt_t));
		if (ckh_insert(bt2cnt, btkey, ret)) {
			idalloc(ret);
			return (NULL);
		}
		malloc_mutex_lock(&ctx->lock);
		ql_tail_insert(&ctx->cnts_ql, ret, link);
		malloc_mutex_unlock(&ctx->lock);
	}

	return (ret);
}

static inline void
prof_sample_threshold_update(void)
{
	uint64_t r;
	double u;

	/*
	 * Compute prof_sample_threshold as a geometrically distributed random
	 * variable with mean (2^opt_lg_prof_sample).
	 */
	prn64(r, 53, prof_sample_prn_state, (uint64_t)1125899906842625LLU,
	    1058392653243283975);
	u = (double)r * (1.0/9007199254740992.0L);
	prof_sample_threshold = (uint64_t)(log(u) /
	    log(1.0 - (1.0 / (double)((uint64_t)1U << opt_lg_prof_sample))))
	    + (uint64_t)1U;
}

prof_thr_cnt_t *
prof_alloc_prep(size_t size)
{
	prof_thr_cnt_t *ret;
	void *vec[prof_bt_max];
	prof_bt_t bt;

	if (opt_prof_active == false) {
		/* Sampling is currently inactive, so avoid sampling. */
		ret = (prof_thr_cnt_t *)(uintptr_t)1U;
	} else if (opt_lg_prof_sample == 0) {
		/*
		 * Don't bother with sampling logic, since sampling interval is
		 * 1.
		 */
		bt_init(&bt, vec);
		prof_backtrace(&bt, 2, prof_bt_max);
		ret = prof_lookup(&bt);
	} else {
		if (prof_sample_threshold == 0) {
			/*
			 * Initialize.  Seed the prng differently for each
			 * thread.
			 */
			prof_sample_prn_state = (uint64_t)(uintptr_t)&size;
			prof_sample_threshold_update();
		}

		/*
		 * Determine whether to capture a backtrace based on whether
		 * size is enough for prof_accum to reach
		 * prof_sample_threshold.  However, delay updating these
		 * variables until prof_{m,re}alloc(), because we don't know
		 * for sure that the allocation will succeed.
		 *
		 * Use subtraction rather than addition to avoid potential
		 * integer overflow.
		 */
		if (size >= prof_sample_threshold - prof_sample_accum) {
			bt_init(&bt, vec);
			prof_backtrace(&bt, 2, prof_bt_max);
			ret = prof_lookup(&bt);
		} else
			ret = (prof_thr_cnt_t *)(uintptr_t)1U;
	}

	return (ret);
}

prof_thr_cnt_t *
prof_cnt_get(const void *ptr)
{
	prof_thr_cnt_t *ret;
	arena_chunk_t *chunk;

	assert(ptr != NULL);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if (chunk != ptr) {
		/* Region. */
		assert(chunk->arena->magic == ARENA_MAGIC);

		ret = arena_prof_cnt_get(ptr);
	} else
		ret = huge_prof_cnt_get(ptr);

	return (ret);
}

static void
prof_cnt_set(const void *ptr, prof_thr_cnt_t *cnt)
{
	arena_chunk_t *chunk;

	assert(ptr != NULL);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if (chunk != ptr) {
		/* Region. */
		assert(chunk->arena->magic == ARENA_MAGIC);

		arena_prof_cnt_set(ptr, cnt);
	} else
		huge_prof_cnt_set(ptr, cnt);
}

static inline void
prof_sample_accum_update(size_t size)
{

	if (opt_lg_prof_sample == 0) {
		/*
		 * Don't bother with sampling logic, since sampling interval is
		 * 1.
		 */
		return;
	}

	/* Take care to avoid integer overflow. */
	if (size >= prof_sample_threshold - prof_sample_accum) {
		prof_sample_accum -= (prof_sample_threshold - size);
		/* Compute new prof_sample_threshold. */
		prof_sample_threshold_update();
		while (prof_sample_accum >= prof_sample_threshold) {
			prof_sample_accum -= prof_sample_threshold;
			prof_sample_threshold_update();
		}
	} else
		prof_sample_accum += size;
}

void
prof_malloc(const void *ptr, prof_thr_cnt_t *cnt)
{
	size_t size = isalloc(ptr);

	assert(ptr != NULL);

	prof_cnt_set(ptr, cnt);
	prof_sample_accum_update(size);

	if ((uintptr_t)cnt > (uintptr_t)1U) {
		cnt->epoch++;
		/*********/
		mb_write();
		/*********/
		cnt->cnts.curobjs++;
		cnt->cnts.curbytes += size;
		cnt->cnts.accumobjs++;
		cnt->cnts.accumbytes += size;
		/*********/
		mb_write();
		/*********/
		cnt->epoch++;
		/*********/
		mb_write();
		/*********/
	}
}

void
prof_realloc(const void *ptr, prof_thr_cnt_t *cnt, const void *old_ptr,
    size_t old_size, prof_thr_cnt_t *old_cnt)
{
	size_t size = isalloc(ptr);

	if (ptr != NULL) {
		prof_cnt_set(ptr, cnt);
		prof_sample_accum_update(size);
	}

	if ((uintptr_t)old_cnt > (uintptr_t)1U)
		old_cnt->epoch++;
	if ((uintptr_t)cnt > (uintptr_t)1U)
		cnt->epoch++;
	/*********/
	mb_write();
	/*********/
	if ((uintptr_t)old_cnt > (uintptr_t)1U) {
		old_cnt->cnts.curobjs--;
		old_cnt->cnts.curbytes -= old_size;
	}
	if ((uintptr_t)cnt > (uintptr_t)1U) {
		cnt->cnts.curobjs++;
		cnt->cnts.curbytes += size;
		cnt->cnts.accumobjs++;
		cnt->cnts.accumbytes += size;
	}
	/*********/
	mb_write();
	/*********/
	if ((uintptr_t)old_cnt > (uintptr_t)1U)
		old_cnt->epoch++;
	if ((uintptr_t)cnt > (uintptr_t)1U)
		cnt->epoch++;
	/*********/
	mb_write(); /* Not strictly necessary. */
}

void
prof_free(const void *ptr)
{
	prof_thr_cnt_t *cnt = prof_cnt_get(ptr);

	if ((uintptr_t)cnt > (uintptr_t)1) {
		size_t size = isalloc(ptr);

		cnt->epoch++;
		/*********/
		mb_write();
		/*********/
		cnt->cnts.curobjs--;
		cnt->cnts.curbytes -= size;
		/*********/
		mb_write();
		/*********/
		cnt->epoch++;
		/*********/
		mb_write();
		/*********/
	}
}

static bool
prof_flush(bool propagate_err)
{
	bool ret = false;
	ssize_t err;

	err = write(prof_dump_fd, prof_dump_buf, prof_dump_buf_end);
	if (err == -1) {
		if (propagate_err == false) {
			malloc_write("<jemalloc>: write() failed during heap "
			    "profile flush\n");
			if (opt_abort)
				abort();
		}
		ret = true;
	}
	prof_dump_buf_end = 0;

	return (ret);
}

static bool
prof_write(const char *s, bool propagate_err)
{
	unsigned i, slen, n;

	i = 0;
	slen = strlen(s);
	while (i < slen) {
		/* Flush the buffer if it is full. */
		if (prof_dump_buf_end == PROF_DUMP_BUF_SIZE)
			if (prof_flush(propagate_err) && propagate_err)
				return (true);

		if (prof_dump_buf_end + slen <= PROF_DUMP_BUF_SIZE) {
			/* Finish writing. */
			n = slen - i;
		} else {
			/* Write as much of s as will fit. */
			n = PROF_DUMP_BUF_SIZE - prof_dump_buf_end;
		}
		memcpy(&prof_dump_buf[prof_dump_buf_end], &s[i], n);
		prof_dump_buf_end += n;
		i += n;
	}

	return (false);
}

static void
prof_ctx_merge(prof_ctx_t *ctx, prof_cnt_t *cnt_all, size_t *leak_nctx)
{
	prof_thr_cnt_t *thr_cnt;
	prof_cnt_t tcnt;

	malloc_mutex_lock(&ctx->lock);

	memcpy(&ctx->cnt_dump, &ctx->cnt_merged, sizeof(prof_cnt_t));
	ql_foreach(thr_cnt, &ctx->cnts_ql, link) {
		volatile unsigned *epoch = &thr_cnt->epoch;

		while (true) {
			unsigned epoch0 = *epoch;

			/* Make sure epoch is even. */
			if (epoch0 & 1U)
				continue;

			memcpy(&tcnt, &thr_cnt->cnts, sizeof(prof_cnt_t));

			/* Terminate if epoch didn't change while reading. */
			if (*epoch == epoch0)
				break;
		}

		ctx->cnt_dump.curobjs += tcnt.curobjs;
		ctx->cnt_dump.curbytes += tcnt.curbytes;
		ctx->cnt_dump.accumobjs += tcnt.accumobjs;
		ctx->cnt_dump.accumbytes += tcnt.accumbytes;

		if (tcnt.curobjs != 0)
			(*leak_nctx)++;
	}

	/* Merge into cnt_all. */
	cnt_all->curobjs += ctx->cnt_dump.curobjs;
	cnt_all->curbytes += ctx->cnt_dump.curbytes;
	cnt_all->accumobjs += ctx->cnt_dump.accumobjs;
	cnt_all->accumbytes += ctx->cnt_dump.accumbytes;

	malloc_mutex_unlock(&ctx->lock);
}

static bool
prof_dump_ctx(prof_ctx_t *ctx, prof_bt_t *bt, bool propagate_err)
{
	char buf[UMAX2S_BUFSIZE];
	unsigned i;

	if (prof_write(umax2s(ctx->cnt_dump.curobjs, 10, buf), propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(umax2s(ctx->cnt_dump.curbytes, 10, buf),
	    propagate_err)
	    || prof_write(" [", propagate_err)
	    || prof_write(umax2s(ctx->cnt_dump.accumobjs, 10, buf),
	    propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(umax2s(ctx->cnt_dump.accumbytes, 10, buf),
	    propagate_err)
	    || prof_write("] @", propagate_err))
		return (true);

	for (i = 0; i < bt->len; i++) {
		if (prof_write(" 0x", propagate_err)
		    || prof_write(umax2s((uintptr_t)bt->vec[i], 16, buf),
		    propagate_err))
			return (true);
	}

	if (prof_write("\n", propagate_err))
		return (true);

	return (false);
}

static bool
prof_dump_maps(bool propagate_err)
{
	int mfd;
	char buf[UMAX2S_BUFSIZE];
	char *s;
	unsigned i, slen;
	/*         /proc/<pid>/maps\0 */
	char mpath[6     + UMAX2S_BUFSIZE
			      + 5  + 1];

	i = 0;

	s = "/proc/";
	slen = strlen(s);
	memcpy(&mpath[i], s, slen);
	i += slen;

	s = umax2s(getpid(), 10, buf);
	slen = strlen(s);
	memcpy(&mpath[i], s, slen);
	i += slen;

	s = "/maps";
	slen = strlen(s);
	memcpy(&mpath[i], s, slen);
	i += slen;

	mpath[i] = '\0';

	mfd = open(mpath, O_RDONLY);
	if (mfd != -1) {
		ssize_t nread;

		if (prof_write("\nMAPPED_LIBRARIES:\n", propagate_err) &&
		    propagate_err)
			return (true);
		nread = 0;
		do {
			prof_dump_buf_end += nread;
			if (prof_dump_buf_end == PROF_DUMP_BUF_SIZE) {
				/* Make space in prof_dump_buf before read(). */
				if (prof_flush(propagate_err) && propagate_err)
					return (true);
			}
			nread = read(mfd, &prof_dump_buf[prof_dump_buf_end],
			    PROF_DUMP_BUF_SIZE - prof_dump_buf_end);
		} while (nread > 0);
		close(mfd);
	} else
		return (true);

	return (false);
}

static bool
prof_dump(const char *filename, bool leakcheck, bool propagate_err)
{
	prof_cnt_t cnt_all;
	size_t tabind;
	prof_bt_t *bt;
	prof_ctx_t *ctx;
	char buf[UMAX2S_BUFSIZE];
	size_t leak_nctx;

	prof_enter();
	prof_dump_fd = creat(filename, 0644);
	if (prof_dump_fd == -1) {
		if (propagate_err == false) {
			malloc_write("<jemalloc>: creat(\"");
			malloc_write(filename);
			malloc_write("\", 0644) failed\n");
			if (opt_abort)
				abort();
		}
		goto ERROR;
	}

	/* Merge per thread profile stats, and sum them in cnt_all. */
	memset(&cnt_all, 0, sizeof(prof_cnt_t));
	leak_nctx = 0;
	for (tabind = 0; ckh_iter(&bt2ctx, &tabind, NULL, (void **)&ctx)
	    == false;) {
		prof_ctx_merge(ctx, &cnt_all, &leak_nctx);
	}

	/* Dump profile header. */
	if (prof_write("heap profile: ", propagate_err)
	    || prof_write(umax2s(cnt_all.curobjs, 10, buf), propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(umax2s(cnt_all.curbytes, 10, buf), propagate_err)
	    || prof_write(" [", propagate_err)
	    || prof_write(umax2s(cnt_all.accumobjs, 10, buf), propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(umax2s(cnt_all.accumbytes, 10, buf), propagate_err))
		goto ERROR;

	if (opt_lg_prof_sample == 0) {
		if (prof_write("] @ heapprofile\n", propagate_err))
			goto ERROR;
	} else {
		if (prof_write("] @ heap_v2/", propagate_err)
		    || prof_write(umax2s((uint64_t)1U << opt_lg_prof_sample, 10,
		    buf), propagate_err)
		    || prof_write("\n", propagate_err))
			goto ERROR;
	}

	/* Dump  per ctx profile stats. */
	for (tabind = 0; ckh_iter(&bt2ctx, &tabind, (void **)&bt, (void **)&ctx)
	    == false;) {
		if (prof_dump_ctx(ctx, bt, propagate_err))
			goto ERROR;
	}

	/* Dump /proc/<pid>/maps if possible. */
	if (prof_dump_maps(propagate_err))
		goto ERROR;

	if (prof_flush(propagate_err))
		goto ERROR;
	close(prof_dump_fd);
	prof_leave();

	if (leakcheck && cnt_all.curbytes != 0) {
		malloc_write("<jemalloc>: Leak summary: ");
		malloc_write(umax2s(cnt_all.curbytes, 10, buf));
		malloc_write((cnt_all.curbytes != 1) ? " bytes, " : " byte, ");
		malloc_write(umax2s(cnt_all.curobjs, 10, buf));
		malloc_write((cnt_all.curobjs != 1) ? " objects, " :
		    " object, ");
		malloc_write(umax2s(leak_nctx, 10, buf));
		malloc_write((leak_nctx != 1) ? " contexts\n" : " context\n");
		malloc_write("<jemalloc>: Run pprof on \"");
		malloc_write(filename);
		malloc_write("\" for leak detail\n");
	}

	return (false);
ERROR:
	prof_leave();
	return (true);
}

#define	DUMP_FILENAME_BUFSIZE	(PATH_MAX+ UMAX2S_BUFSIZE		\
					       + 1			\
						+ UMAX2S_BUFSIZE	\
						     + 2		\
						       + UMAX2S_BUFSIZE	\
						             + 5  + 1)
static void
prof_dump_filename(char *filename, char v, int64_t vseq)
{
	char buf[UMAX2S_BUFSIZE];
	char *s;
	unsigned i, slen;

	/*
	 * Construct a filename of the form:
	 *
	 *   <prefix>.<pid>.<seq>.v<vseq>.heap\0
	 * or
	 *   jeprof.<pid>.<seq>.v<vseq>.heap\0
	 */

	i = 0;

	/*
	 * Use JEMALLOC_PROF_PREFIX if it's set, and if it is short enough to
	 * avoid overflowing DUMP_FILENAME_BUFSIZE.  The result may exceed
	 * PATH_MAX, but creat(2) will catch that problem.
	 */
	if ((s = getenv("JEMALLOC_PROF_PREFIX")) != NULL
	    && strlen(s) + (DUMP_FILENAME_BUFSIZE - PATH_MAX) <= PATH_MAX) {
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;

		s = ".";
	} else
		s = "jeprof.";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = umax2s(getpid(), 10, buf);
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = ".";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = umax2s(prof_dump_seq, 10, buf);
	prof_dump_seq++;
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = ".";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	filename[i] = v;
	i++;

	if (vseq != 0xffffffffffffffffLLU) {
		s = umax2s(vseq, 10, buf);
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;
	}

	s = ".heap";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	filename[i] = '\0';
}

static void
prof_fdump(void)
{
	char filename[DUMP_FILENAME_BUFSIZE];

	if (prof_booted == false)
		return;

	malloc_mutex_lock(&prof_dump_seq_mtx);
	prof_dump_filename(filename, 'f', 0xffffffffffffffffLLU);
	malloc_mutex_unlock(&prof_dump_seq_mtx);
	prof_dump(filename, opt_prof_leak, false);
}

void
prof_idump(void)
{
	char filename[DUMP_FILENAME_BUFSIZE];

	if (prof_booted == false)
		return;
	malloc_mutex_lock(&enq_mtx);
	if (enq) {
		enq_idump = true;
		malloc_mutex_unlock(&enq_mtx);
		return;
	}
	malloc_mutex_unlock(&enq_mtx);

	malloc_mutex_lock(&prof_dump_seq_mtx);
	prof_dump_filename(filename, 'i', prof_dump_iseq);
	prof_dump_iseq++;
	malloc_mutex_unlock(&prof_dump_seq_mtx);
	prof_dump(filename, false, false);
}

bool
prof_mdump(const char *filename)
{
	char filename_buf[DUMP_FILENAME_BUFSIZE];

	if (opt_prof == false || prof_booted == false)
		return (true);

	if (filename == NULL) {
		/* No filename specified, so automatically generate one. */
		malloc_mutex_lock(&prof_dump_seq_mtx);
		prof_dump_filename(filename_buf, 'm', prof_dump_mseq);
		prof_dump_mseq++;
		malloc_mutex_unlock(&prof_dump_seq_mtx);
		filename = filename_buf;
	}
	return (prof_dump(filename, false, true));
}

void
prof_udump(void)
{
	char filename[DUMP_FILENAME_BUFSIZE];

	if (prof_booted == false)
		return;
	malloc_mutex_lock(&enq_mtx);
	if (enq) {
		enq_udump = true;
		malloc_mutex_unlock(&enq_mtx);
		return;
	}
	malloc_mutex_unlock(&enq_mtx);

	malloc_mutex_lock(&prof_dump_seq_mtx);
	prof_dump_filename(filename, 'u', prof_dump_useq);
	prof_dump_useq++;
	malloc_mutex_unlock(&prof_dump_seq_mtx);
	prof_dump(filename, false, false);
}

static void
prof_bt_hash(const void *key, unsigned minbits, size_t *hash1, size_t *hash2)
{
	size_t ret1, ret2;
	uint64_t h;
	prof_bt_t *bt = (prof_bt_t *)key;

	assert(minbits <= 32 || (SIZEOF_PTR == 8 && minbits <= 64));
	assert(hash1 != NULL);
	assert(hash2 != NULL);

	h = hash(bt->vec, bt->len * sizeof(void *), 0x94122f335b332aeaLLU);
	if (minbits <= 32) {
		/*
		 * Avoid doing multiple hashes, since a single hash provides
		 * enough bits.
		 */
		ret1 = h & ZU(0xffffffffU);
		ret2 = h >> 32;
	} else {
		ret1 = h;
		ret2 = hash(bt->vec, bt->len * sizeof(void *),
		    0x8432a476666bbc13U);
	}

	*hash1 = ret1;
	*hash2 = ret2;
}

static bool
prof_bt_keycomp(const void *k1, const void *k2)
{
	const prof_bt_t *bt1 = (prof_bt_t *)k1;
	const prof_bt_t *bt2 = (prof_bt_t *)k2;

	if (bt1->len != bt2->len)
		return (false);
	return (memcmp(bt1->vec, bt2->vec, bt1->len * sizeof(void *)) == 0);
}

static void
bt2cnt_thread_cleanup(void *arg)
{
	ckh_t *bt2cnt;

	bt2cnt = bt2cnt_tls;
	if (bt2cnt != NULL) {
		ql_head(prof_thr_cnt_t) cnts_ql;
		size_t tabind;
		prof_thr_cnt_t *cnt;

		/* Iteratively merge cnt's into the global stats. */
		ql_new(&cnts_ql);
		tabind = 0;
		while (ckh_iter(bt2cnt, &tabind, NULL, (void **)&cnt) ==
		    false) {
			prof_ctx_t *ctx = cnt->ctx;
			/* Merge stats and detach from ctx. */
			malloc_mutex_lock(&ctx->lock);
			ctx->cnt_merged.curobjs += cnt->cnts.curobjs;
			ctx->cnt_merged.curbytes += cnt->cnts.curbytes;
			ctx->cnt_merged.accumobjs += cnt->cnts.accumobjs;
			ctx->cnt_merged.accumbytes += cnt->cnts.accumbytes;
			ql_remove(&ctx->cnts_ql, cnt, link);
			malloc_mutex_unlock(&ctx->lock);

			/*
			 * Stash cnt for deletion after finishing with
			 * ckh_iter().
			 */
			ql_tail_insert(&cnts_ql, cnt, link);
		}

		/*
		 * Delete the hash table now that cnts_ql has a list of all
		 * cnt's.
		 */
		ckh_delete(bt2cnt);
		idalloc(bt2cnt);
		bt2cnt_tls = NULL;

		/* Delete cnt's. */
		while ((cnt = ql_last(&cnts_ql, link)) != NULL) {
			ql_remove(&cnts_ql, cnt, link);
			idalloc(cnt);
		}
	}
}

void
prof_boot0(void)
{

	/*
	 * opt_prof and prof_promote must be in their final state before any
	 * arenas are initialized, so this function must be executed early.
	 */

	if (opt_prof_leak && opt_prof == false) {
		/*
		 * Enable opt_prof, but in such a way that profiles are never
		 * automatically dumped.
		 */
		opt_prof = true;
		opt_prof_udump = false;
		prof_interval = 0;
	} else if (opt_prof) {
		if (opt_lg_prof_interval >= 0) {
			prof_interval = (((uint64_t)1U) <<
			    opt_lg_prof_interval);
		} else
			prof_interval = 0;
	}

	prof_promote = (opt_prof && opt_lg_prof_sample > PAGE_SHIFT);
}

bool
prof_boot1(void)
{

	if (opt_prof) {
		if (ckh_new(&bt2ctx, PROF_CKH_MINITEMS, prof_bt_hash,
		    prof_bt_keycomp))
			return (true);
		if (malloc_mutex_init(&bt2ctx_mtx))
			return (true);
		if (pthread_key_create(&bt2cnt_tsd, bt2cnt_thread_cleanup)
		    != 0) {
			malloc_write(
			    "<jemalloc>: Error in pthread_key_create()\n");
			abort();
		}

		prof_bt_max = (1U << opt_lg_prof_bt_max);
		if (malloc_mutex_init(&prof_dump_seq_mtx))
			return (true);

		if (malloc_mutex_init(&enq_mtx))
			return (true);
		enq = false;
		enq_idump = false;
		enq_udump = false;

		if (atexit(prof_fdump) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort)
				abort();
		}
	}

#ifdef JEMALLOC_PROF_LIBGCC
	/*
	 * Cause the backtracing machinery to allocate its internal state
	 * before enabling profiling.
	 */
	_Unwind_Backtrace(prof_unwind_init_callback, NULL);
#endif

	prof_booted = true;

	return (false);
}

/******************************************************************************/
#endif /* JEMALLOC_PROF */
