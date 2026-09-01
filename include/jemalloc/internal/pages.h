#ifndef JEMALLOC_INTERNAL_PAGES_EXTERNS_H
#define JEMALLOC_INTERNAL_PAGES_EXTERNS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/jemalloc_internal_types.h"

/* Actual operating system page size, detected during bootstrap, <= PAGE. */
extern size_t os_page;

#ifdef DYNAMIC_PAGE_SIZE
#	undef LG_PAGE
#	ifdef JEMALLOC_DEBUG
#		define LG_PAGE get_lg_page_size()
#		define PAGE get_page_size()
#	else /* JEMALLOC_DEBUG */
#		define LG_PAGE lg_page_size
#		define PAGE page_size
#	endif /* JEMALLOC_DEBUG */
#else /* DYNAMIC_PAGE_SIZE */
/* Page size.  LG_PAGE is determined by the configure script. */
#	define PAGE ((size_t)(1U << LG_PAGE))
#endif /* DYNAMIC_PAGE_SIZE */

#ifdef PAGE_MASK
#	undef PAGE_MASK
#endif
#define PAGE_MASK ((size_t)(PAGE - 1))
/* Return the page base address for the page containing address a. */
#define PAGE_ADDR2BASE(a) ALIGNMENT_ADDR2BASE(a, PAGE)
/* Return the smallest pagesize multiple that is >= s. */
#define PAGE_CEILING(s) (((s) + PAGE_MASK) & ~PAGE_MASK)
/* Return the largest pagesize multiple that is <=s. */
#define PAGE_FLOOR(s) ((s) & ~PAGE_MASK)

#ifdef DYNAMIC_PAGE_SIZE
#	define MIN_PAGE ((size_t)(1U << MIN_LG_PAGE))
#	define MAX_PAGE ((size_t)(1U << MAX_LG_PAGE))

/*
 * Either MIN/MAX (dynamic page size) or LG_PAGE (no dynamic page size).
 * This pattern is required in a few places. Using these macros
 * eliminates the need to add more #ifdef's in the code.
 */
#	define LG_PAGE_OR_MIN MIN_LG_PAGE
#	define LG_PAGE_OR_MAX MAX_LG_PAGE

/* log of page size, only used when dynamic page size is enabled */
extern unsigned lg_page_size;

/* page size based on lg_page_size, only used when dynamic page size is enabled */
extern size_t page_size;

/* number of pages per hugepage, only used when dynamic page size is enabled */
extern size_t hugepage_pages;

#	ifdef JEMALLOC_DEBUG
/* Used in debug builds to check that the globals have been initialized. */

JEMALLOC_ALWAYS_INLINE unsigned
get_lg_page_size() {
	assert(lg_page_size != 0);
	return lg_page_size;
}

JEMALLOC_ALWAYS_INLINE size_t
get_page_size() {
	assert(page_size != 0);
	return page_size;
}

JEMALLOC_ALWAYS_INLINE size_t
get_hugepage_pages() {
	assert(hugepage_pages != 0);
	return hugepage_pages;
}
#	endif /* JEMALLOC_DEBUG */
#else          /* DYNAMIC_PAGE_SIZE */
#	define LG_PAGE_OR_MIN LG_PAGE
#	define LG_PAGE_OR_MAX LG_PAGE
#endif /* DYNAMIC_PAGE_SIZE */

/* Huge page size.  LG_HUGEPAGE is determined by the configure script. */
#define HUGEPAGE ((size_t)(1U << LG_HUGEPAGE))
#define HUGEPAGE_MASK ((size_t)(HUGEPAGE - 1))

/*
 * Used to validate that the hugepage size is not unexpectedly high.  The huge
 * page features (HPA, metadata_thp) are primarily designed with a 2M THP size
 * in mind.  Much larger sizes are not tested and likely to cause issues such as
 * bad fragmentation or simply broken.
 */
#define HUGEPAGE_MAX_EXPECTED_SIZE ((size_t)(16U << 20))

#if LG_HUGEPAGE != 0
#	ifdef DYNAMIC_PAGE_SIZE
#		ifdef JEMALLOC_DEBUG
#			define HUGEPAGE_PAGES get_hugepage_pages()
#		else /* JEMALLOC_DEBUG */
#			define HUGEPAGE_PAGES hugepage_pages
#		endif /* JEMALLOC_DEBUG */
#	else /* DYNAMIC_PAGE_SIZE */
#		define HUGEPAGE_PAGES (HUGEPAGE / PAGE)
#	endif /* DYNAMIC_PAGE_SIZE */
#	define HUGEPAGE_PAGES_MAX (HUGEPAGE >> LG_PAGE_OR_MIN)
#else
/*
 * It's convenient to define arrays (or bitmaps) of HUGEPAGE_PAGES lengths.  If
 * we can't autodetect the hugepage size, it gets treated as 0, in which case
 * we'll trigger a compiler error in those arrays.  Avoid this case by ensuring
 * that this value is at least 1.  (We won't ever run in this degraded state;
 * hpa_supported() returns false in this case.
 */
#	define HUGEPAGE_PAGES 1
#	define HUGEPAGE_PAGES_MAX HUGEPAGE_PAGES
#endif

/* Return the huge page base address for the huge page containing address a. */
#define HUGEPAGE_ADDR2BASE(a) ALIGNMENT_ADDR2BASE(a, HUGEPAGE)
/* Return the smallest pagesize multiple that is >= s. */
#define HUGEPAGE_CEILING(s) (((s) + HUGEPAGE_MASK) & ~HUGEPAGE_MASK)

/* PAGES_CAN_PURGE_LAZY is defined if lazy purging is supported. */
#if defined(_WIN32) || defined(JEMALLOC_PURGE_MADVISE_FREE)
#	define PAGES_CAN_PURGE_LAZY
#endif
/*
 * PAGES_CAN_PURGE_FORCED is defined if forced purging is supported.
 *
 * The only supported way to hard-purge on Windows is to decommit and then
 * re-commit, but doing so is racy, and if re-commit fails it's a pain to
 * propagate the "poisoned" memory state.  Since we typically decommit as the
 * next step after purging on Windows anyway, there's no point in adding such
 * complexity.
 */
#if !defined(_WIN32)                                                           \
    && ((defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                              \
            && defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS))                 \
        || defined(JEMALLOC_MAPS_COALESCE))
#	define PAGES_CAN_PURGE_FORCED
#endif

static const bool pages_can_purge_lazy =
#ifdef PAGES_CAN_PURGE_LAZY
    true
#else
    false
#endif
    ;
static const bool pages_can_purge_forced =
#ifdef PAGES_CAN_PURGE_FORCED
    true
#else
    false
#endif
    ;

#if defined(JEMALLOC_HAVE_MADVISE_HUGE) || defined(JEMALLOC_HAVE_MEMCNTL)
#	define PAGES_CAN_HUGIFY
#endif

static const bool pages_can_hugify =
#ifdef PAGES_CAN_HUGIFY
    true
#else
    false
#endif
    ;

/*
 * thp_mode_t are values for opt.thp, while system_thp_mode_t is for kernel thp
 * settings, i.e., init_system_thp_mode.
 */
typedef enum {
	thp_mode_do_nothing = 0, /* Respect kernel thp settings. */
	thp_mode_always = 1,  /* Always set MADV_HUGEPAGE. */
	thp_mode_never = 2,   /* Always set MADV_NOHUGEPAGE. */

	thp_mode_names_limit = 3,  /* Used for option processing. */
	thp_mode_not_supported = 3 /* No THP support detected. */
} thp_mode_t;

typedef enum {
	system_thp_mode_madvise = 0,     /* Kernel THP mode: madvise */
	system_thp_mode_always = 1,      /* Kernel THP mode: always */
	system_thp_mode_never = 2,       /* Kernel THP mode: never */
	system_thp_mode_not_supported = 3 /* No THP support detected. */
} system_thp_mode_t;

#define THP_MODE_DEFAULT thp_mode_do_nothing
extern thp_mode_t        opt_thp;
extern system_thp_mode_t init_system_thp_mode; /* Initial system wide state. */
extern const char *const thp_mode_names[];
extern const char *const system_thp_mode_names[];

void *pages_map(void *addr, size_t size, size_t alignment, bool *commit);
void  pages_unmap(void *addr, size_t size);
bool  pages_commit(void *addr, size_t size);
bool  pages_decommit(void *addr, size_t size);
bool  pages_purge_lazy(void *addr, size_t size);
bool  pages_purge_forced(void *addr, size_t size);
bool pages_purge_process_madvise(void *vec, size_t ven_len, size_t total_bytes);
bool pages_huge(void *addr, size_t size);
bool pages_nohuge(void *addr, size_t size);
bool pages_collapse(void *addr, size_t size);
bool pages_dontdump(void *addr, size_t size);
bool pages_dodump(void *addr, size_t size);
bool pages_pre_boot(void);
bool pages_boot(void);
void pages_set_thp_state(void *ptr, size_t size);
void pages_mark_guards(void *head, void *tail);
void pages_unmark_guards(void *head, void *tail);

#endif /* JEMALLOC_INTERNAL_PAGES_EXTERNS_H */
