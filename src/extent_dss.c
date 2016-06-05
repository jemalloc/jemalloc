#define	JEMALLOC_EXTENT_DSS_C_
#include "jemalloc/internal/jemalloc_internal.h"
/******************************************************************************/
/* Data. */

const char	*dss_prec_names[] = {
	"disabled",
	"primary",
	"secondary",
	"N/A"
};

/* Current dss precedence default, used when creating new arenas. */
static dss_prec_t	dss_prec_default = DSS_PREC_DEFAULT;

/*
 * Protects sbrk() calls.  This avoids malloc races among threads, though it
 * does not protect against races with threads that call sbrk() directly.
 */
static malloc_mutex_t	dss_mtx;

/* Base address of the DSS. */
static void		*dss_base;
/* Current end of the DSS, or ((void *)-1) if the DSS is exhausted. */
static void		*dss_prev;
/* Current upper limit on DSS addresses. */
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
extent_dss_prec_get(tsdn_t *tsdn)
{
	dss_prec_t ret;

	if (!have_dss)
		return (dss_prec_disabled);
	malloc_mutex_lock(tsdn, &dss_mtx);
	ret = dss_prec_default;
	malloc_mutex_unlock(tsdn, &dss_mtx);
	return (ret);
}

bool
extent_dss_prec_set(tsdn_t *tsdn, dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	malloc_mutex_lock(tsdn, &dss_mtx);
	dss_prec_default = dss_prec;
	malloc_mutex_unlock(tsdn, &dss_mtx);
	return (false);
}

void *
extent_alloc_dss(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit)
{
	void *ret;
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

	malloc_mutex_lock(tsdn, &dss_mtx);
	if (dss_prev != (void *)-1) {
		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		while (true) {
			void *gap_addr, *dss_next;
			size_t gap_size;
			intptr_t incr;

			/* Avoid an unnecessary system call. */
			if (new_addr != NULL && dss_max != new_addr)
				break;

			/* Get the current end of the DSS. */
			dss_max = extent_dss_sbrk(0);

			/* Make sure the earlier condition still holds. */
			if (new_addr != NULL && dss_max != new_addr)
				break;

			/*
			 * Compute how much gap space (if any) is necessary to
			 * satisfy alignment.  This space can be recycled for
			 * later use.
			 */
			gap_addr = (void *)(PAGE_CEILING((uintptr_t)dss_max));
			ret = (void *)ALIGNMENT_CEILING((uintptr_t)gap_addr,
			    PAGE_CEILING(alignment));
			gap_size = (uintptr_t)ret - (uintptr_t)gap_addr;
			if (gap_size != 0) {
				extent_init(gap, arena, gap_addr, gap_size,
				    gap_size, false, false, true, false);
			}
			dss_next = (void *)((uintptr_t)ret + size);
			if ((uintptr_t)ret < (uintptr_t)dss_max ||
			    (uintptr_t)dss_next < (uintptr_t)dss_max)
				break; /* Wrap-around. */
			incr = gap_size + size;
			dss_prev = extent_dss_sbrk(incr);
			if (dss_prev == (void *)-1)
				break;
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = dss_next;
				malloc_mutex_unlock(tsdn, &dss_mtx);
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
		}
	}
	/* OOM. */
	malloc_mutex_unlock(tsdn, &dss_mtx);
	extent_dalloc(tsdn, arena, gap);
	return (NULL);
}

bool
extent_in_dss(tsdn_t *tsdn, void *addr)
{
	bool ret;

	cassert(have_dss);

	malloc_mutex_lock(tsdn, &dss_mtx);
	if ((uintptr_t)addr >= (uintptr_t)dss_base
	    && (uintptr_t)addr < (uintptr_t)dss_max)
		ret = true;
	else
		ret = false;
	malloc_mutex_unlock(tsdn, &dss_mtx);

	return (ret);
}

bool
extent_dss_boot(void)
{

	cassert(have_dss);

	if (malloc_mutex_init(&dss_mtx, "dss", WITNESS_RANK_DSS))
		return (true);
	dss_base = extent_dss_sbrk(0);
	dss_prev = dss_base;
	dss_max = dss_base;

	return (false);
}

void
extent_dss_prefork(tsdn_t *tsdn)
{

	if (have_dss)
		malloc_mutex_prefork(tsdn, &dss_mtx);
}

void
extent_dss_postfork_parent(tsdn_t *tsdn)
{

	if (have_dss)
		malloc_mutex_postfork_parent(tsdn, &dss_mtx);
}

void
extent_dss_postfork_child(tsdn_t *tsdn)
{

	if (have_dss)
		malloc_mutex_postfork_child(tsdn, &dss_mtx);
}

/******************************************************************************/
