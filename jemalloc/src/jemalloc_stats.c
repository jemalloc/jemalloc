#define	JEMALLOC_STATS_C_
#include "internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

bool	opt_stats_print = false;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void
malloc_vcprintf(void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque, const char *format, va_list ap);

/******************************************************************************/

/*
 * We don't want to depend on vsnprintf() for production builds, since that can
 * cause unnecessary bloat for static binaries.  umax2s() provides minimal
 * integer printing functionality, so that malloc_printf() use can be limited to
 * JEMALLOC_STATS code.
 */
char *
umax2s(uintmax_t x, unsigned base, char *s)
{
	unsigned i;

	i = UMAX2S_BUFSIZE - 1;
	s[i] = '\0';
	switch (base) {
	case 10:
		do {
			i--;
			s[i] = "0123456789"[x % 10];
			x /= 10;
		} while (x > 0);
		break;
	case 16:
		do {
			i--;
			s[i] = "0123456789abcdef"[x & 0xf];
			x >>= 4;
		} while (x > 0);
		break;
	default:
		do {
			i--;
			s[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[x % base];
			x /= base;
		} while (x > 0);
	}

	return (&s[i]);
}

#ifdef JEMALLOC_STATS
static void
malloc_vcprintf(void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque, const char *format, va_list ap)
{
	char buf[4096];

	if (write4 == NULL) {
		/*
		 * The caller did not provide an alternate write4 callback
		 * function, so use the default one.  malloc_write4() is an
		 * inline function, so use malloc_message() directly here.
		 */
		write4 = JEMALLOC_P(malloc_message);
		w4opaque = NULL;
	}

	vsnprintf(buf, sizeof(buf), format, ap);
	write4(w4opaque, buf, "", "", "");
}

/*
 * Print to a callback function in such a way as to (hopefully) avoid memory
 * allocation.
 */
JEMALLOC_ATTR(format(printf, 3, 4))
void
malloc_cprintf(void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(write4, w4opaque, format, ap);
	va_end(ap);
}

/*
 * Print to stderr in such a way as to (hopefully) avoid memory allocation.
 */
JEMALLOC_ATTR(format(printf, 1, 2))
void
malloc_printf(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
}
#endif

void
stats_print(void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque, const char *opts)
{
	char s[UMAX2S_BUFSIZE];
	bool general = true;
	bool merged = true;
	bool unmerged = true;
	bool bins = true;
	bool large = true;

	if (write4 == NULL) {
		/*
		 * The caller did not provide an alternate write4 callback
		 * function, so use the default one.  malloc_write4() is an
		 * inline function, so use malloc_message() directly here.
		 */
		write4 = JEMALLOC_P(malloc_message);
		w4opaque = NULL;
	}

	if (opts != NULL) {
		unsigned i;

		for (i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
				case 'g':
					general = false;
					break;
				case 'm':
					merged = false;
					break;
				case 'a':
					unmerged = false;
					break;
				case 'b':
					bins = false;
					break;
				case 'l':
					large = false;
					break;
				default:;
			}
		}
	}

	write4(w4opaque, "___ Begin jemalloc statistics ___\n", "", "", "");
	if (general) {
		write4(w4opaque, "Assertions ",
#ifdef NDEBUG
		    "disabled",
#else
		    "enabled",
#endif
		    "\n", "");
		write4(w4opaque, "Boolean JEMALLOC_OPTIONS: ",
		    opt_abort ? "A" : "a", "", "");
#ifdef JEMALLOC_FILL
		write4(w4opaque, opt_junk ? "J" : "j", "", "", "");
#endif
#ifdef JEMALLOC_SWAP
		write4(w4opaque, opt_overcommit ? "O" : "o", "", "", "");
#endif
		write4(w4opaque, "P", "", "", "");
#ifdef JEMALLOC_TCACHE
		write4(w4opaque, opt_tcache_sort ? "S" : "s", "", "", "");
#endif
#ifdef JEMALLOC_TRACE
		write4(w4opaque, opt_trace ? "T" : "t", "", "", "");
#endif
#ifdef JEMALLOC_SYSV
		write4(w4opaque, opt_sysv ? "V" : "v", "", "", "");
#endif
#ifdef JEMALLOC_XMALLOC
		write4(w4opaque, opt_xmalloc ? "X" : "x", "", "", "");
#endif
#ifdef JEMALLOC_FILL
		write4(w4opaque, opt_zero ? "Z" : "z", "", "", "");
#endif
		write4(w4opaque, "\n", "", "", "");

		write4(w4opaque, "CPUs: ", umax2s(ncpus, 10, s), "\n", "");
		write4(w4opaque, "Max arenas: ", umax2s(narenas, 10, s), "\n",
		    "");
		write4(w4opaque, "Pointer size: ", umax2s(sizeof(void *), 10,
		    s), "\n", "");
		write4(w4opaque, "Quantum size: ", umax2s(QUANTUM, 10, s), "\n",
		    "");
		write4(w4opaque, "Cacheline size (assumed): ", umax2s(CACHELINE,
		    10, s),
		    "\n", "");
		write4(w4opaque, "Subpage spacing: ", umax2s(SUBPAGE, 10, s),
		    "\n", "");
		write4(w4opaque, "Medium spacing: ", umax2s((1U << lg_mspace),
		    10, s), "\n", "");
#ifdef JEMALLOC_TINY
		write4(w4opaque, "Tiny 2^n-spaced sizes: [", umax2s((1U <<
		    LG_TINY_MIN), 10, s), "..", "");
		write4(w4opaque, umax2s((qspace_min >> 1), 10, s), "]\n", "",
		    "");
#endif
		write4(w4opaque, "Quantum-spaced sizes: [", umax2s(qspace_min,
		    10, s), "..", "");
		write4(w4opaque, umax2s(qspace_max, 10, s), "]\n", "", "");
		write4(w4opaque, "Cacheline-spaced sizes: [", umax2s(cspace_min,
		    10, s), "..", "");
		write4(w4opaque, umax2s(cspace_max, 10, s), "]\n", "", "");
		write4(w4opaque, "Subpage-spaced sizes: [", umax2s(sspace_min,
		    10, s), "..", "");
		write4(w4opaque, umax2s(sspace_max, 10, s), "]\n", "", "");
		write4(w4opaque, "Medium sizes: [", umax2s(medium_min, 10, s),
		    "..", "");
		write4(w4opaque, umax2s(medium_max, 10, s), "]\n", "", "");
		if (opt_lg_dirty_mult >= 0) {
			write4(w4opaque,
			    "Min active:dirty page ratio per arena: ",
			    umax2s((1U << opt_lg_dirty_mult), 10, s), ":1\n",
			    "");
		} else {
			write4(w4opaque,
			    "Min active:dirty page ratio per arena: N/A\n", "",
			    "", "");
		}
#ifdef JEMALLOC_TCACHE
		write4(w4opaque, "Thread cache slots per size class: ",
		    tcache_nslots ? umax2s(tcache_nslots, 10, s) : "N/A", "\n",
		    "");
		write4(w4opaque, "Thread cache GC sweep interval: ",
		    (tcache_nslots && tcache_gc_incr > 0) ?
		    umax2s((1U << opt_lg_tcache_gc_sweep), 10, s) : "N/A",
		    "", "");
		write4(w4opaque, " (increment interval: ",
		    (tcache_nslots && tcache_gc_incr > 0) ?
		    umax2s(tcache_gc_incr, 10, s) : "N/A",
		    ")\n", "");
#endif
		write4(w4opaque, "Chunk size: ", umax2s(chunksize, 10, s), "",
		    "");
		write4(w4opaque, " (2^", umax2s(opt_lg_chunk, 10, s), ")\n",
		    "");
	}

#ifdef JEMALLOC_STATS
	{
		size_t allocated, mapped;
		unsigned i;
		arena_t *arena;

		/* Calculate and print allocated/mapped stats. */

		/* arenas. */
		for (i = 0, allocated = 0; i < narenas; i++) {
			if (arenas[i] != NULL) {
				malloc_mutex_lock(&arenas[i]->lock);
				allocated += arenas[i]->stats.allocated_small;
				allocated += arenas[i]->stats.allocated_large;
				malloc_mutex_unlock(&arenas[i]->lock);
			}
		}

		/* huge/base. */
		malloc_mutex_lock(&huge_mtx);
		allocated += huge_allocated;
		mapped = stats_chunks.curchunks * chunksize;
		malloc_mutex_unlock(&huge_mtx);

		malloc_cprintf(write4, w4opaque,
		    "Allocated: %zu, mapped: %zu\n", allocated, mapped);

		/* Print chunk stats. */
		{
			chunk_stats_t chunks_stats;
#ifdef JEMALLOC_SWAP
			size_t swap_avail_chunks;
#endif

			malloc_mutex_lock(&huge_mtx);
			chunks_stats = stats_chunks;
			malloc_mutex_unlock(&huge_mtx);

#ifdef JEMALLOC_SWAP
			malloc_mutex_lock(&swap_mtx);
			swap_avail_chunks = swap_avail >> opt_lg_chunk;
			malloc_mutex_unlock(&swap_mtx);
#endif

			malloc_cprintf(write4, w4opaque, "chunks: nchunks   "
			    "highchunks    curchunks"
#ifdef JEMALLOC_SWAP
			    "   swap_avail"
#endif
			    "\n");
			malloc_cprintf(write4, w4opaque,
			    "  %13"PRIu64"%13zu%13zu"
#ifdef JEMALLOC_SWAP
			    "%13zu"
#endif
			    "\n",
			    chunks_stats.nchunks, chunks_stats.highchunks,
			    chunks_stats.curchunks
#ifdef JEMALLOC_SWAP
			    , swap_avail_chunks
#endif
			    );
		}

		/* Print chunk stats. */
		malloc_cprintf(write4, w4opaque,
		    "huge: nmalloc      ndalloc    allocated\n");
		malloc_cprintf(write4, w4opaque,
		    " %12"PRIu64" %12"PRIu64" %12zu\n",
		    huge_nmalloc, huge_ndalloc, huge_allocated);

		if (merged) {
			size_t nactive, ndirty;
			arena_stats_t astats;
			malloc_bin_stats_t bstats[nbins];
			malloc_large_stats_t lstats[((chunksize - PAGE_SIZE) >>
			    PAGE_SHIFT)];

			nactive = 0;
			ndirty = 0;
			memset(&astats, 0, sizeof(astats));
			memset(bstats, 0, sizeof(bstats));
			memset(lstats, 0, sizeof(lstats));

			/* Create merged arena stats. */
			for (i = 0; i < narenas; i++) {
				arena = arenas[i];
				if (arena != NULL) {
					malloc_mutex_lock(&arena->lock);
					arena_stats_merge(arena, &nactive,
					    &ndirty, &astats, bstats, lstats);
					malloc_mutex_unlock(&arena->lock);
				}
			}
			/* Print merged arena stats. */
			malloc_cprintf(write4, w4opaque,
			    "\nMerge arenas stats:\n");
			/* arenas[0] is used only for invariant bin settings. */
			arena_stats_mprint(arenas[0], nactive, ndirty, &astats,
			    bstats, lstats, bins, large, write4, w4opaque);
		}

		if (unmerged) {
			/* Print stats for each arena. */
			for (i = 0; i < narenas; i++) {
				arena = arenas[i];
				if (arena != NULL) {
					malloc_cprintf(write4, w4opaque,
					    "\narenas[%u]:\n", i);
					malloc_mutex_lock(&arena->lock);
					arena_stats_print(arena, bins, large,
					    write4, w4opaque);
					malloc_mutex_unlock(&arena->lock);
				}
			}
		}
	}
#endif /* #ifdef JEMALLOC_STATS */
	write4(w4opaque, "--- End jemalloc statistics ---\n", "", "", "");
}
