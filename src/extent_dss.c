#define	JEMALLOC_EXTENT_DSS_C_
#include "jemalloc/internal/jemalloc_internal.h"
/******************************************************************************/
/* Data. */

const char	*opt_dss = DSS_DEFAULT;

const char	*dss_prec_names[] = {
	"disabled",
	"primary",
	"secondary",
	"N/A"
};

/*
 * Current dss precedence default, used when creating new arenas.  NB: This is
 * stored as unsigned rather than dss_prec_t because in principle there's no
 * guarantee that sizeof(dss_prec_t) is the same as sizeof(unsigned), and we use
 * atomic operations to synchronize the setting.
 */
static unsigned		dss_prec_default = (unsigned)DSS_PREC_DEFAULT;

/* Base address of the DSS. */
static void		*dss_base;
/* Atomic boolean indicating whether the DSS is exhausted. */
static unsigned		dss_exhausted;
/* Atomic current upper limit on DSS addresses. */
static void		*dss_max;

/******************************************************************************/

static void *
extent_dss_sbrk(intptr_t increment)
{

#ifdef JEMALLOC_DSS
	return (sbrk(increment));
#else
	not_implemented();
	return (NULL);
#endif
}

dss_prec_t
extent_dss_prec_get(void)
{
	dss_prec_t ret;

	if (!have_dss)
		return (dss_prec_disabled);
	ret = (dss_prec_t)atomic_read_u(&dss_prec_default);
	return (ret);
}

bool
extent_dss_prec_set(dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	atomic_write_u(&dss_prec_default, (unsigned)dss_prec);
	return (false);
}

static void *
extent_dss_max_update(void *new_addr)
{
	void *max_cur;
	spin_t spinner;

	/*
	 * Get the current end of the DSS as max_cur and assure that dss_max is
	 * up to date.
	 */
	spin_init(&spinner);
	while (true) {
		void *max_prev = atomic_read_p(&dss_max);

		max_cur = extent_dss_sbrk(0);
		if ((uintptr_t)max_prev > (uintptr_t)max_cur) {
			/*
			 * Another thread optimistically updated dss_max.  Wait
			 * for it to finish.
			 */
			spin_adaptive(&spinner);
			continue;
		}
		if (!atomic_cas_p(&dss_max, max_prev, max_cur))
			break;
	}
	/* Fixed new_addr can only be supported if it is at the edge of DSS. */
	if (new_addr != NULL && max_cur != new_addr)
		return (NULL);

	return (max_cur);
}

void *
extent_alloc_dss(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit)
{
	extent_t *gap;

	cassert(have_dss);
	assert(size > 0);
	assert(alignment > 0);

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a large allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	gap = extent_alloc(tsdn, arena);
	if (gap == NULL)
		return (NULL);

	if (!atomic_read_u(&dss_exhausted)) {
		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		while (true) {
			void *ret, *max_cur, *gap_addr, *dss_next, *dss_prev;
			size_t gap_size;
			intptr_t incr;

			max_cur = extent_dss_max_update(new_addr);
			if (max_cur == NULL)
				goto label_oom;

			/*
			 * Compute how much gap space (if any) is necessary to
			 * satisfy alignment.  This space can be recycled for
			 * later use.
			 */
			gap_addr = (void *)(PAGE_CEILING((uintptr_t)max_cur));
			ret = (void *)ALIGNMENT_CEILING((uintptr_t)gap_addr,
			    PAGE_CEILING(alignment));
			gap_size = (uintptr_t)ret - (uintptr_t)gap_addr;
			if (gap_size != 0) {
				extent_init(gap, arena, gap_addr, gap_size,
				    gap_size, false, false, true, false);
			}
			dss_next = (void *)((uintptr_t)ret + size);
			if ((uintptr_t)ret < (uintptr_t)max_cur ||
			    (uintptr_t)dss_next < (uintptr_t)max_cur)
				goto label_oom; /* Wrap-around. */
			incr = gap_size + size;

			/*
			 * Optimistically update dss_max, and roll back below if
			 * sbrk() fails.  No other thread will try to extend the
			 * DSS while dss_max is greater than the current DSS
			 * max reported by sbrk(0).
			 */
			if (atomic_cas_p(&dss_max, max_cur, dss_next))
				continue;

			/* Try to allocate. */
			dss_prev = extent_dss_sbrk(incr);
			if (dss_prev == max_cur) {
				/* Success. */
				if (gap_size != 0)
					extent_dalloc_gap(tsdn, arena, gap);
				else
					extent_dalloc(tsdn, arena, gap);
				if (*zero)
					memset(ret, 0, size);
				if (!*commit)
					*commit = pages_decommit(ret, size);
				return (ret);
			}
			/*
			 * Failure, whether due to OOM or a race with a raw
			 * sbrk() call from outside the allocator.  Try to roll
			 * back optimistic dss_max update; if rollback fails,
			 * it's due to another caller of this function having
			 * succeeded since this invocation started, in which
			 * case rollback is not necessary.
			 */
			atomic_cas_p(&dss_max, dss_next, max_cur);
			if (dss_prev == (void *)-1) {
				/* OOM. */
				atomic_write_u(&dss_exhausted, (unsigned)true);
				goto label_oom;
			}
		}
	}
label_oom:
	extent_dalloc(tsdn, arena, gap);
	return (NULL);
}

static bool
extent_in_dss_helper(void *addr, void *max)
{

	return ((uintptr_t)addr >= (uintptr_t)dss_base && (uintptr_t)addr <
	    (uintptr_t)max);
}

bool
extent_in_dss(void *addr)
{

	cassert(have_dss);

	return (extent_in_dss_helper(addr, atomic_read_p(&dss_max)));
}

bool
extent_dss_mergeable(void *addr_a, void *addr_b)
{
	void *max;

	cassert(have_dss);

	if ((uintptr_t)addr_a < (uintptr_t)dss_base && (uintptr_t)addr_b <
	    (uintptr_t)dss_base)
		return (true);

	max = atomic_read_p(&dss_max);
	return (extent_in_dss_helper(addr_a, max) ==
	    extent_in_dss_helper(addr_b, max));
}

void
extent_dss_boot(void)
{

	cassert(have_dss);

	dss_base = extent_dss_sbrk(0);
	dss_exhausted = (unsigned)(dss_base == (void *)-1);
	dss_max = dss_base;
}

/******************************************************************************/
