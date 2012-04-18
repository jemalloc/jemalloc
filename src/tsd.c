#define	JEMALLOC_TSD_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

static unsigned ncleanups;
static malloc_tsd_cleanup_t cleanups[MALLOC_TSD_CLEANUPS_MAX];

/******************************************************************************/

void *
malloc_tsd_malloc(size_t size)
{

	/* Avoid choose_arena() in order to dodge bootstrapping issues. */
	return arena_malloc(arenas[0], size, false, false);
}

void
malloc_tsd_dalloc(void *wrapper)
{

	idalloc(wrapper);
}

void
malloc_tsd_no_cleanup(void *arg)
{

	not_reached();
}

#ifdef JEMALLOC_MALLOC_THREAD_CLEANUP
JEMALLOC_ATTR(visibility("default"))
void
_malloc_thread_cleanup(void)
{
	bool pending[ncleanups], again;
	unsigned i;

	for (i = 0; i < ncleanups; i++)
		pending[i] = true;

	do {
		again = false;
		for (i = 0; i < ncleanups; i++) {
			if (pending[i]) {
				pending[i] = cleanups[i]();
				if (pending[i])
					again = true;
			}
		}
	} while (again);
}
#endif

void
malloc_tsd_cleanup_register(bool (*f)(void))
{

	assert(ncleanups < MALLOC_TSD_CLEANUPS_MAX);
	cleanups[ncleanups] = f;
	ncleanups++;
}

void
malloc_tsd_boot(void)
{

	ncleanups = 0;
}
