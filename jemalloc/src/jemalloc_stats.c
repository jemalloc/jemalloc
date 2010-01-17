#define	JEMALLOC_STATS_C_
#include "internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

bool	opt_stats_print = false;

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
/*
 * Print to stderr in such a way as to (hopefully) avoid memory allocation.
 */
void
malloc_printf(const char *format, ...)
{
	char buf[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	malloc_write4(buf, "", "", "");
}
#endif

JEMALLOC_ATTR(visibility("default"))
void
JEMALLOC_P(malloc_stats_print)(const char *opts)
{
	char s[UMAX2S_BUFSIZE];
	bool general = true;
	bool bins = true;
	bool large = true;

	if (opts != NULL) {
		unsigned i;

		for (i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
				case 'g':
					general = false;
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

	malloc_write4("___ Begin jemalloc statistics ___\n", "", "", "");
	if (general) {
		malloc_write4("Assertions ",
#ifdef NDEBUG
		    "disabled",
#else
		    "enabled",
#endif
		    "\n", "");
		malloc_write4("Boolean JEMALLOC_OPTIONS: ",
		    opt_abort ? "A" : "a", "", "");
#ifdef JEMALLOC_FILL
		malloc_write4(opt_junk ? "J" : "j", "", "", "");
#endif
		malloc_write4("P", "", "", "");
#ifdef JEMALLOC_TCACHE
		malloc_write4(opt_tcache_sort ? "S" : "s", "", "", "");
#endif
#ifdef JEMALLOC_TRACE
		malloc_write4(opt_trace ? "T" : "t", "", "", "");
#endif
#ifdef JEMALLOC_SYSV
		malloc_write4(opt_sysv ? "V" : "v", "", "", "");
#endif
#ifdef JEMALLOC_XMALLOC
		malloc_write4(opt_xmalloc ? "X" : "x", "", "", "");
#endif
#ifdef JEMALLOC_FILL
		malloc_write4(opt_zero ? "Z" : "z", "", "", "");
#endif
		malloc_write4("\n", "", "", "");

		malloc_write4("CPUs: ", umax2s(ncpus, 10, s), "\n", "");
		malloc_write4("Max arenas: ", umax2s(narenas, 10, s), "\n", "");
		malloc_write4("Pointer size: ", umax2s(sizeof(void *), 10, s),
		    "\n", "");
		malloc_write4("Quantum size: ", umax2s(QUANTUM, 10, s), "\n",
		    "");
		malloc_write4("Cacheline size (assumed): ",
		    umax2s(CACHELINE, 10, s), "\n", "");
		malloc_write4("Subpage spacing: ", umax2s(SUBPAGE, 10, s),
		    "\n", "");
		malloc_write4("Medium spacing: ", umax2s((1U << lg_mspace), 10,
		    s), "\n", "");
#ifdef JEMALLOC_TINY
		malloc_write4("Tiny 2^n-spaced sizes: [", umax2s((1U <<
		    LG_TINY_MIN), 10, s), "..", "");
		malloc_write4(umax2s((qspace_min >> 1), 10, s), "]\n", "", "");
#endif
		malloc_write4("Quantum-spaced sizes: [", umax2s(qspace_min, 10,
		    s), "..", "");
		malloc_write4(umax2s(qspace_max, 10, s), "]\n", "", "");
		malloc_write4("Cacheline-spaced sizes: [",
		    umax2s(cspace_min, 10, s), "..", "");
		malloc_write4(umax2s(cspace_max, 10, s), "]\n", "", "");
		malloc_write4("Subpage-spaced sizes: [", umax2s(sspace_min, 10,
		    s), "..", "");
		malloc_write4(umax2s(sspace_max, 10, s), "]\n", "", "");
		malloc_write4("Medium sizes: [", umax2s(medium_min, 10, s),
		    "..", "");
		malloc_write4(umax2s(medium_max, 10, s), "]\n", "", "");
		if (opt_lg_dirty_mult >= 0) {
			malloc_write4(
			    "Min active:dirty page ratio per arena: ",
			    umax2s((1U << opt_lg_dirty_mult), 10, s), ":1\n",
			    "");
		} else {
			malloc_write4(
			    "Min active:dirty page ratio per arena: N/A\n",
			    "", "", "");
		}
#ifdef JEMALLOC_TCACHE
		malloc_write4("Thread cache slots per size class: ",
		    tcache_nslots ? umax2s(tcache_nslots, 10, s) : "N/A",
		    "\n", "");
		malloc_write4("Thread cache GC sweep interval: ",
		    (tcache_nslots && tcache_gc_incr > 0) ?
		    umax2s((1U << opt_lg_tcache_gc_sweep), 10, s) : "N/A",
		    "", "");
		malloc_write4(" (increment interval: ",
		    (tcache_nslots && tcache_gc_incr > 0) ?
		    umax2s(tcache_gc_incr, 10, s) : "N/A",
		    ")\n", "");
#endif
		malloc_write4("Chunk size: ", umax2s(chunksize, 10, s), "", "");
		malloc_write4(" (2^", umax2s(opt_lg_chunk, 10, s), ")\n", "");
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

		malloc_mutex_lock(&base_mtx);
		mapped += base_mapped;
		malloc_mutex_unlock(&base_mtx);

		malloc_printf("Allocated: %zu, mapped: %zu\n", allocated,
		    mapped);

		/* Print chunk stats. */
		{
			chunk_stats_t chunks_stats;

			malloc_mutex_lock(&huge_mtx);
			chunks_stats = stats_chunks;
			malloc_mutex_unlock(&huge_mtx);

			malloc_printf("chunks: nchunks   "
			    "highchunks    curchunks\n");
			malloc_printf("  %13llu%13lu%13lu\n",
			    chunks_stats.nchunks, chunks_stats.highchunks,
			    chunks_stats.curchunks);
		}

		/* Print chunk stats. */
		malloc_printf(
		    "huge: nmalloc      ndalloc    allocated\n");
		malloc_printf(" %12llu %12llu %12zu\n", huge_nmalloc,
		    huge_ndalloc, huge_allocated);

		/* Print stats for each arena. */
		for (i = 0; i < narenas; i++) {
			arena = arenas[i];
			if (arena != NULL) {
				malloc_printf("\narenas[%u]:\n", i);
				malloc_mutex_lock(&arena->lock);
				arena_stats_print(arena, bins, large);
				malloc_mutex_unlock(&arena->lock);
			}
		}
	}
#endif /* #ifdef JEMALLOC_STATS */
	malloc_write4("--- End jemalloc statistics ---\n", "", "", "");
}
