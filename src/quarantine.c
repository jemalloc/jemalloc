#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

typedef struct quarantine_s quarantine_t;

struct quarantine_s {
	size_t	curbytes;
	size_t	curobjs;
	size_t	first;
#define	LG_MAXOBJS_INIT 10
	size_t	lg_maxobjs;
	void	*objs[1]; /* Dynamically sized ring buffer. */
};

static void	quarantine_cleanup(void *arg);

malloc_tsd_data(static, quarantine, quarantine_t *, NULL)
malloc_tsd_funcs(JEMALLOC_INLINE, quarantine, quarantine_t *, NULL,
    quarantine_cleanup)

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static quarantine_t	*quarantine_init(size_t lg_maxobjs);
static quarantine_t	*quarantine_grow(quarantine_t *quarantine);
static void	quarantine_drain(quarantine_t *quarantine, size_t upper_bound);

/******************************************************************************/

static quarantine_t *
quarantine_init(size_t lg_maxobjs)
{
	quarantine_t *quarantine;

	quarantine = (quarantine_t *)imalloc(offsetof(quarantine_t, objs) +
	    ((ZU(1) << lg_maxobjs) * sizeof(void *)));
	if (quarantine == NULL)
		return (NULL);
	quarantine->curbytes = 0;
	quarantine->curobjs = 0;
	quarantine->first = 0;
	quarantine->lg_maxobjs = lg_maxobjs;

	quarantine_tsd_set(&quarantine);

	return (quarantine);
}

static quarantine_t *
quarantine_grow(quarantine_t *quarantine)
{
	quarantine_t *ret;

	ret = quarantine_init(quarantine->lg_maxobjs + 1);
	if (ret == NULL)
		return (quarantine);

	ret->curbytes = quarantine->curbytes;
	if (quarantine->first + quarantine->curobjs < (ZU(1) <<
	    quarantine->lg_maxobjs)) {
		/* objs ring buffer data are contiguous. */
		memcpy(ret->objs, &quarantine->objs[quarantine->first],
		    quarantine->curobjs * sizeof(void *));
		ret->curobjs = quarantine->curobjs;
	} else {
		/* objs ring buffer data wrap around. */
		size_t ncopy = (ZU(1) << quarantine->lg_maxobjs) -
		    quarantine->first;
		memcpy(ret->objs, &quarantine->objs[quarantine->first], ncopy *
		    sizeof(void *));
		ret->curobjs = ncopy;
		if (quarantine->curobjs != 0) {
			memcpy(&ret->objs[ret->curobjs], quarantine->objs,
			    quarantine->curobjs - ncopy);
		}
	}

	return (ret);
}

static void
quarantine_drain(quarantine_t *quarantine, size_t upper_bound)
{

	while (quarantine->curbytes > upper_bound && quarantine->curobjs > 0) {
		void *ptr = quarantine->objs[quarantine->first];
		size_t usize = isalloc(ptr, config_prof);
		idalloc(ptr);
		quarantine->curbytes -= usize;
		quarantine->curobjs--;
		quarantine->first = (quarantine->first + 1) & ((ZU(1) <<
		    quarantine->lg_maxobjs) - 1);
	}
}

void
quarantine(void *ptr)
{
	quarantine_t *quarantine;
	size_t usize = isalloc(ptr, config_prof);

	cassert(config_fill);
	assert(opt_quarantine);

	quarantine = *quarantine_tsd_get();
	if (quarantine == NULL && (quarantine =
	    quarantine_init(LG_MAXOBJS_INIT)) == NULL) {
		idalloc(ptr);
		return;
	}
	/*
	 * Drain one or more objects if the quarantine size limit would be
	 * exceeded by appending ptr.
	 */
	if (quarantine->curbytes + usize > opt_quarantine) {
		size_t upper_bound = (opt_quarantine >= usize) ? opt_quarantine
		    - usize : 0;
		quarantine_drain(quarantine, upper_bound);
	}
	/* Grow the quarantine ring buffer if it's full. */
	if (quarantine->curobjs == (ZU(1) << quarantine->lg_maxobjs))
		quarantine = quarantine_grow(quarantine);
	/* quarantine_grow() must free a slot if it fails to grow. */
	assert(quarantine->curobjs < (ZU(1) << quarantine->lg_maxobjs));
	/* Append ptr if its size doesn't exceed the quarantine size. */
	if (quarantine->curbytes + usize <= opt_quarantine) {
		size_t offset = (quarantine->first + quarantine->curobjs) &
		    ((ZU(1) << quarantine->lg_maxobjs) - 1);
		quarantine->objs[offset] = ptr;
		quarantine->curbytes += usize;
		quarantine->curobjs++;
		if (opt_junk)
			memset(ptr, 0x5a, usize);
	} else {
		assert(quarantine->curbytes == 0);
		idalloc(ptr);
	}
}

static void
quarantine_cleanup(void *arg)
{
	quarantine_t *quarantine = *(quarantine_t **)arg;

	if (quarantine != NULL) {
		quarantine_drain(quarantine, 0);
		idalloc(quarantine);
	}
}

bool
quarantine_boot(void)
{

	cassert(config_fill);

	if (quarantine_tsd_boot())
		return (true);

	return (false);
}
