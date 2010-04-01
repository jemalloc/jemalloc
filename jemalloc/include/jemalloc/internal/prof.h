#ifdef JEMALLOC_PROF
/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct prof_bt_s prof_bt_t;
typedef struct prof_cnt_s prof_cnt_t;
typedef struct prof_thr_cnt_s prof_thr_cnt_t;
typedef struct prof_ctx_s prof_ctx_t;
typedef struct prof_s prof_t;

/* Option defaults. */
#define	LG_PROF_BT_MAX_DEFAULT		2
#define	LG_PROF_SAMPLE_DEFAULT		0
#define	LG_PROF_INTERVAL_DEFAULT	30

/*
 * Hard limit on stack backtrace depth.  Note that the version of
 * prof_backtrace() that is based on __builtin_return_address() necessarily has
 * a hard-coded number of backtrace frame handlers, so increasing
 * LG_PROF_BT_MAX requires changing prof_backtrace().
 */
#define	LG_PROF_BT_MAX		7 /* >= LG_PROF_BT_MAX_DEFAULT */
#define	PROF_BT_MAX		(1U << LG_PROF_BT_MAX)

/* Initial hash table size. */
#define	PROF_CKH_MINITEMS	64

/* Size of memory buffer to use when writing dump files. */
#define	PROF_DUMP_BUF_SIZE	65536

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct prof_bt_s {
	/* Backtrace, stored as len program counters. */
	void			**vec;
	unsigned		len;
};

#ifdef JEMALLOC_PROF_LIBGCC
/* Data structure passed to libgcc _Unwind_Backtrace() callback functions. */
typedef struct {
	prof_bt_t *bt;
	unsigned nignore;
	unsigned max;
} prof_unwind_data_t;
#endif

struct prof_cnt_s {
	/*
	 * Profiling counters.  An allocation/deallocation pair can operate on
	 * different prof_thr_cnt_t objects that are linked into the same
	 * prof_ctx_t sets_ql, so it is possible for the cur* counters to go
	 * negative.  In principle it is possible for the *bytes counters to
	 * overflow/underflow, but a general solution would require some form
	 * of 128-bit counter solution; this implementation doesn't bother to
	 * solve that problem.
	 */
	int64_t		curobjs;
	int64_t		curbytes;
	uint64_t	accumobjs;
	uint64_t	accumbytes;
};

struct prof_thr_cnt_s {
	/* Linkage into prof_ctx_t's sets_ql. */
	ql_elm(prof_thr_cnt_t)	link;

	/*
	 * Associated context.  If a thread frees an object that it did not
	 * allocate, it is possible that the context is not cached in the
	 * thread's hash table, in which case it must be able to look up the
	 * context, insert a new prof_thr_cnt_t into the thread's hash table,
	 * and link it into the prof_ctx_t's sets_ql.
	 */
	prof_ctx_t		*ctx;

	/*
	 * Threads use memory barriers to update the counters.  Since there is
	 * only ever one writer, the only challenge is for the reader to get a
	 * consistent read of the counters.
	 *
	 * The writer uses this series of operations:
	 *
	 * 1) Increment epoch to an odd number.
	 * 2) Update counters.
	 * 3) Increment epoch to an even number.
	 *
	 * The reader must assure 1) that the epoch is even while it reads the
	 * counters, and 2) that the epoch doesn't change between the time it
	 * starts and finishes reading the counters.
	 */
	unsigned		epoch;

	/* Profiling counters. */
	prof_cnt_t		cnts;
};

struct prof_ctx_s {
	/* Protects cnt_merged and sets_ql. */
	malloc_mutex_t		lock;

	/* Temporary storage for aggregation during dump. */
	prof_cnt_t		cnt_dump;

	/* When threads exit, they merge their stats into cnt_merged. */
	prof_cnt_t		cnt_merged;

	/*
	 * List of profile counters, one for each thread that has allocated in
	 * this context.
	 */
	ql_head(prof_thr_cnt_t)	cnts_ql;
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern bool	opt_prof;
/*
 * Even if opt_prof is true, sampling can be temporarily disabled by setting
 * opt_prof_active to false.  No locking is used when updating opt_prof_active,
 * so there are no guarantees regarding how long it will take for all threads
 * to notice state changes.
 */
extern bool	opt_prof_active;
extern size_t	opt_lg_prof_bt_max; /* Maximum backtrace depth. */
extern size_t	opt_lg_prof_sample; /* Mean bytes between samples. */
extern ssize_t	opt_lg_prof_interval; /* lg(prof_interval). */
extern bool	opt_prof_udump; /* High-water memory dumping. */
extern bool	opt_prof_leak; /* Dump leak summary at exit. */

/*
 * Profile dump interval, measured in bytes allocated.  Each arena triggers a
 * profile dump when it reaches this threshold.  The effect is that the
 * interval between profile dumps averages prof_interval, though the actual
 * interval between dumps will tend to be sporadic, and the interval will be a
 * maximum of approximately (prof_interval * narenas).
 */
extern uint64_t	prof_interval;

/*
 * If true, promote small sampled objects to large objects, since small run
 * headers do not have embedded profile context pointers.
 */
extern bool	prof_promote;

bool	prof_init(prof_t *prof, bool master);
void	prof_destroy(prof_t *prof);

prof_thr_cnt_t	*prof_alloc_prep(size_t size);
prof_thr_cnt_t	*prof_cnt_get(const void *ptr);
void	prof_malloc(const void *ptr, prof_thr_cnt_t *cnt);
void	prof_realloc(const void *ptr, prof_thr_cnt_t *cnt, const void *old_ptr,
    size_t old_size, prof_thr_cnt_t *old_cnt);
void	prof_free(const void *ptr);
void	prof_idump(void);
bool	prof_mdump(const char *filename);
void	prof_udump(void);
void	prof_boot0(void);
bool	prof_boot1(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
#endif /* JEMALLOC_PROF */
