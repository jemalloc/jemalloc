/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct witness_s witness_t;
typedef unsigned witness_rank_t;
typedef ql_head(witness_t) witness_list_t;
typedef int witness_comp_t (const witness_t *, const witness_t *);

/*
 * Lock ranks.  Witnesses with rank WITNESS_RANK_OMIT are completely ignored by
 * the witness machinery.
 */
#define	WITNESS_RANK_OMIT		0U

#define	WITNESS_RANK_INIT		1U
#define	WITNESS_RANK_CTL		1U
#define	WITNESS_RANK_ARENAS		2U

#define	WITNESS_RANK_PROF_DUMP		3U
#define	WITNESS_RANK_PROF_BT2GCTX	4U
#define	WITNESS_RANK_PROF_TDATAS	5U
#define	WITNESS_RANK_PROF_TDATA		6U
#define	WITNESS_RANK_PROF_GCTX		7U

#define	WITNESS_RANK_ARENA		8U
#define	WITNESS_RANK_ARENA_CHUNKS	9U
#define	WITNESS_RANK_ARENA_NODE_CACHE	10

#define	WITNESS_RANK_BASE		11U

#define	WITNESS_RANK_LEAF		0xffffffffU
#define	WITNESS_RANK_ARENA_BIN		WITNESS_RANK_LEAF
#define	WITNESS_RANK_ARENA_HUGE		WITNESS_RANK_LEAF
#define	WITNESS_RANK_DSS		WITNESS_RANK_LEAF
#define	WITNESS_RANK_PROF_ACTIVE	WITNESS_RANK_LEAF
#define	WITNESS_RANK_PROF_DUMP_SEQ	WITNESS_RANK_LEAF
#define	WITNESS_RANK_PROF_GDUMP		WITNESS_RANK_LEAF
#define	WITNESS_RANK_PROF_NEXT_THR_UID	WITNESS_RANK_LEAF
#define	WITNESS_RANK_PROF_THREAD_ACTIVE_INIT	WITNESS_RANK_LEAF

#define	WITNESS_INITIALIZER(rank) {"initializer", rank, NULL, {NULL, NULL}}

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct witness_s {
	/* Name, used for printing lock order reversal messages. */
	const char		*name;

	/*
	 * Witness rank, where 0 is lowest and UINT_MAX is highest.  Witnesses
	 * must be acquired in order of increasing rank.
	 */
	witness_rank_t		rank;

	/*
	 * If two witnesses are of equal rank and they have the samp comp
	 * function pointer, it is called as a last attempt to differentiate
	 * between witnesses of equal rank.
	 */
	witness_comp_t		*comp;

	/* Linkage for thread's currently owned locks. */
	ql_elm(witness_t)	link;
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	witness_init(witness_t *witness, const char *name, witness_rank_t rank,
    witness_comp_t *comp);
#ifdef JEMALLOC_JET
typedef void (witness_lock_error_t)(const witness_list_t *, const witness_t *);
extern witness_lock_error_t *witness_lock_error;
#endif
void	witness_lock(tsd_t *tsd, witness_t *witness);
void	witness_unlock(tsd_t *tsd, witness_t *witness);
#ifdef JEMALLOC_JET
typedef void (witness_owner_error_t)(const witness_t *);
extern witness_owner_error_t *witness_owner_error;
#endif
void	witness_assert_owner(tsd_t *tsd, const witness_t *witness);
#ifdef JEMALLOC_JET
typedef void (witness_not_owner_error_t)(const witness_t *);
extern witness_not_owner_error_t *witness_not_owner_error;
#endif
void	witness_assert_not_owner(tsd_t *tsd, const witness_t *witness);
#ifdef JEMALLOC_JET
typedef void (witness_lockless_error_t)(const witness_list_t *);
extern witness_lockless_error_t *witness_lockless_error;
#endif
void	witness_assert_lockless(tsd_t *tsd);

void	witnesses_cleanup(tsd_t *tsd);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
