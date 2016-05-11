#define	JEMALLOC_WITNESS_C_
#include "jemalloc/internal/jemalloc_internal.h"

void
witness_init(witness_t *witness, const char *name, witness_rank_t rank,
    witness_comp_t *comp)
{

	witness->name = name;
	witness->rank = rank;
	witness->comp = comp;
}

#ifdef JEMALLOC_JET
#undef witness_lock_error
#define	witness_lock_error JEMALLOC_N(witness_lock_error_impl)
#endif
static void
witness_lock_error(const witness_list_t *witnesses, const witness_t *witness)
{
	witness_t *w;

	malloc_printf("<jemalloc>: Lock rank order reversal:");
	ql_foreach(w, witnesses, link) {
		malloc_printf(" %s(%u)", w->name, w->rank);
	}
	malloc_printf(" %s(%u)\n", witness->name, witness->rank);
	abort();
}
#ifdef JEMALLOC_JET
#undef witness_lock_error
#define	witness_lock_error JEMALLOC_N(witness_lock_error)
witness_lock_error_t *witness_lock_error = JEMALLOC_N(witness_lock_error_impl);
#endif

void
witness_lock(tsdn_t *tsdn, witness_t *witness)
{
	tsd_t *tsd;
	witness_list_t *witnesses;
	witness_t *w;

	if (tsdn_null(tsdn))
		return;
	tsd = tsdn_tsd(tsdn);
	if (witness->rank == WITNESS_RANK_OMIT)
		return;

	witness_assert_not_owner(tsdn, witness);

	witnesses = tsd_witnessesp_get(tsd);
	w = ql_last(witnesses, link);
	if (w == NULL) {
		/* No other locks; do nothing. */
	} else if (tsd_witness_fork_get(tsd) && w->rank <= witness->rank) {
		/* Forking, and relaxed ranking satisfied. */
	} else if (w->rank > witness->rank) {
		/* Not forking, rank order reversal. */
		witness_lock_error(witnesses, witness);
	} else if (w->rank == witness->rank && (w->comp == NULL || w->comp !=
	    witness->comp || w->comp(w, witness) > 0)) {
		/*
		 * Missing/incompatible comparison function, or comparison
		 * function indicates rank order reversal.
		 */
		witness_lock_error(witnesses, witness);
	}

	ql_elm_new(witness, link);
	ql_tail_insert(witnesses, witness, link);
}

void
witness_unlock(tsdn_t *tsdn, witness_t *witness)
{
	tsd_t *tsd;
	witness_list_t *witnesses;

	if (tsdn_null(tsdn))
		return;
	tsd = tsdn_tsd(tsdn);
	if (witness->rank == WITNESS_RANK_OMIT)
		return;

	witness_assert_owner(tsdn, witness);

	witnesses = tsd_witnessesp_get(tsd);
	ql_remove(witnesses, witness, link);
}

#ifdef JEMALLOC_JET
#undef witness_owner_error
#define	witness_owner_error JEMALLOC_N(witness_owner_error_impl)
#endif
static void
witness_owner_error(const witness_t *witness)
{

	malloc_printf("<jemalloc>: Should own %s(%u)\n", witness->name,
	    witness->rank);
	abort();
}
#ifdef JEMALLOC_JET
#undef witness_owner_error
#define	witness_owner_error JEMALLOC_N(witness_owner_error)
witness_owner_error_t *witness_owner_error =
    JEMALLOC_N(witness_owner_error_impl);
#endif

void
witness_assert_owner(tsdn_t *tsdn, const witness_t *witness)
{
	tsd_t *tsd;
	witness_list_t *witnesses;
	witness_t *w;

	if (tsdn_null(tsdn))
		return;
	tsd = tsdn_tsd(tsdn);
	if (witness->rank == WITNESS_RANK_OMIT)
		return;

	witnesses = tsd_witnessesp_get(tsd);
	ql_foreach(w, witnesses, link) {
		if (w == witness)
			return;
	}
	witness_owner_error(witness);
}

#ifdef JEMALLOC_JET
#undef witness_not_owner_error
#define	witness_not_owner_error JEMALLOC_N(witness_not_owner_error_impl)
#endif
static void
witness_not_owner_error(const witness_t *witness)
{

	malloc_printf("<jemalloc>: Should not own %s(%u)\n", witness->name,
	    witness->rank);
	abort();
}
#ifdef JEMALLOC_JET
#undef witness_not_owner_error
#define	witness_not_owner_error JEMALLOC_N(witness_not_owner_error)
witness_not_owner_error_t *witness_not_owner_error =
    JEMALLOC_N(witness_not_owner_error_impl);
#endif

void
witness_assert_not_owner(tsdn_t *tsdn, const witness_t *witness)
{
	tsd_t *tsd;
	witness_list_t *witnesses;
	witness_t *w;

	if (tsdn_null(tsdn))
		return;
	tsd = tsdn_tsd(tsdn);
	if (witness->rank == WITNESS_RANK_OMIT)
		return;

	witnesses = tsd_witnessesp_get(tsd);
	ql_foreach(w, witnesses, link) {
		if (w == witness)
			witness_not_owner_error(witness);
	}
}

#ifdef JEMALLOC_JET
#undef witness_lockless_error
#define	witness_lockless_error JEMALLOC_N(witness_lockless_error_impl)
#endif
static void
witness_lockless_error(const witness_list_t *witnesses)
{
	witness_t *w;

	malloc_printf("<jemalloc>: Should not own any locks:");
	ql_foreach(w, witnesses, link) {
		malloc_printf(" %s(%u)", w->name, w->rank);
	}
	malloc_printf("\n");
	abort();
}
#ifdef JEMALLOC_JET
#undef witness_lockless_error
#define	witness_lockless_error JEMALLOC_N(witness_lockless_error)
witness_lockless_error_t *witness_lockless_error =
    JEMALLOC_N(witness_lockless_error_impl);
#endif

void
witness_assert_lockless(tsdn_t *tsdn)
{
	tsd_t *tsd;
	witness_list_t *witnesses;
	witness_t *w;

	if (tsdn_null(tsdn))
		return;
	tsd = tsdn_tsd(tsdn);

	witnesses = tsd_witnessesp_get(tsd);
	w = ql_last(witnesses, link);
	if (w != NULL) {
		witness_lockless_error(witnesses);
	}
}

void
witnesses_cleanup(tsd_t *tsd)
{

	witness_assert_lockless(tsd_tsdn(tsd));

	/* Do nothing. */
}

void
witness_fork_cleanup(tsd_t *tsd)
{

	/* Do nothing. */
}

void
witness_prefork(tsd_t *tsd)
{

	tsd_witness_fork_set(tsd, true);
}

void
witness_postfork_parent(tsd_t *tsd)
{

	tsd_witness_fork_set(tsd, false);
}

void
witness_postfork_child(tsd_t *tsd)
{
#ifndef JEMALLOC_MUTEX_INIT_CB
	witness_list_t *witnesses;

	witnesses = tsd_witnessesp_get(tsd);
	ql_new(witnesses);
#endif
	tsd_witness_fork_set(tsd, false);
}
