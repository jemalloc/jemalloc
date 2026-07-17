#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/background_thread.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emitter.h"
#include "jemalloc/internal/fxp.h"
#include "jemalloc/internal/mutex_prof.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/prof_inlines.h"
#include "jemalloc/internal/prof_stats.h"
#include "jemalloc/internal/stats_internal.h"
#include "jemalloc/internal/tcache.h"

static const char *const global_mutex_names[mutex_prof_num_global_mutexes] = {
#define OP(mtx) #mtx,
    MUTEX_PROF_GLOBAL_MUTEXES
#undef OP
};

static const char *const arena_mutex_names[mutex_prof_num_arena_mutexes] = {
#define OP(mtx) #mtx,
    MUTEX_PROF_ARENA_MUTEXES
#undef OP
};

/******************************************************************************/
/* Data. */

bool opt_stats_print = false;
char opt_stats_print_opts[stats_print_tot_num_options + 1] = "";

int64_t opt_stats_interval = STATS_INTERVAL_DEFAULT;
char    opt_stats_interval_opts[stats_print_tot_num_options + 1] = "";

static counter_accum_t stats_interval_accumulated;
/* Per thread batch accum size for stats_interval. */
uint64_t stats_interval_accum_batch;

/*
 * Forward declarations for the two top-level section helpers.  The public
 * stats_print() appears first (just below) and calls these; their definitions
 * follow it, so the file reads top-down.
 */
static void stats_general_print(emitter_t *emitter, bool omit_size_class_meta);
static void stats_print_runtime_stats(emitter_t *emitter, bool merged,
    bool destroyed,
    bool unmerged, bool bins, bool large, bool mutex, bool extents, bool hpa);

/******************************************************************************/
/*
 * malloc_stats_print() output structure (produced by stats_print(), below);
 * each line names the function that prints it, plus its JSON key where useful.
 * Quoted names are literal keys below the "jemalloc" root unless shown with a
 * leading dot.
 *
 *   ___ Begin jemalloc statistics ___
 *   { "jemalloc":
 *     general [g]               -> stats_general_print
 *       version                 -> stats_general_version
 *       config                  -> stats_general_config
 *       system                  -> stats_general_system
 *       opt.*                   -> stats_general_opts
 *       prof                    -> stats_general_prof
 *       arenas metadata         -> stats_general_arenas_print      ("arenas")
 *     stats  (config_stats)     -> stats_print_runtime_stats
 *       global totals           -> stats_print_globals              ("stats")
 *       background_thread       -> stats_print_globals
 *       mutexes [x]             -> stats_global_mutexes_print
 *       per arena [m]/[d]/[a]   -> stats_arena_print          ("stats.arenas")
 *         basics                -> stats_arena_basics_print
 *         decay                 -> stats_arena_decay_print
 *         alloc counts          -> stats_arena_alloc_print   (.small/.large)
 *         memory                -> stats_arena_mem_print
 *         pac/sec               -> stats_arena_pac_sec_print
 *         mutexes [x]           -> stats_arena_mutexes_print    (.mutexes)
 *         bins [b]              -> stats_arena_bins_print       (.bins[j])
 *         lextents [l]          -> stats_arena_lextents_print   (.lextents[j])
 *         extents [e]           -> stats_arena_extents_print    (.extents[j])
 *         hpa shard [h]         -> stats_arena_hpa_shard_print  (.hpa_shard)
 *   }
 *   --- End jemalloc statistics ---
 *
 * Most refactored sections read values through mallctl into typed structs
 * before rendering them through the emitter.  This makes their coverage easier
 * to audit, but does not guarantee parity between formats: format-specific
 * emission remains, including JSON-only geometry and table-only derived rates
 * and summary rows.
 *
 * Mallctl names identify data sources, not mechanical JSON paths.  For example,
 * stats_gather_arena_alloc reads "stats.arenas.0.small.allocated" (replacing
 * the MIB arena index at runtime), while the emitter writes it below
 * jemalloc["stats.arenas"][<arena>].small.allocated.  In particular, "stats"
 * and "stats.arenas" are sibling keys, not nested objects.
 */

void
stats_print(write_cb_t *write_cb, void *cbopaque, const char *opts) {
	int      err;
	uint64_t epoch;
	size_t   u64sz;
#define OPTION(o, v, d, s) bool v = d;
	STATS_PRINT_OPTIONS
#undef OPTION

	/*
	 * Refresh stats, in case mallctl() was called by the application.
	 *
	 * Check for OOM here, since refreshing the ctl cache can trigger
	 * allocation.  In practice, none of the subsequent mallctl()-related
	 * calls in this function will cause OOM if this one succeeds.
	 * */
	epoch = 1;
	u64sz = sizeof(uint64_t);
	err = je_mallctl(
	    "epoch", (void *)&epoch, &u64sz, (void *)&epoch, sizeof(uint64_t));
	if (err != 0) {
		if (err == EAGAIN) {
			malloc_write(
			    "<jemalloc>: Memory allocation failure in "
			    "mallctl(\"epoch\", ...)\n");
			return;
		}
		malloc_write(
		    "<jemalloc>: Failure in mallctl(\"epoch\", "
		    "...)\n");
		abort();
	}

	if (opts != NULL) {
		for (unsigned i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
#define OPTION(o, v, d, s)                                                     \
	case o:                                                                \
		v = s;                                                         \
		break;
				STATS_PRINT_OPTIONS
#undef OPTION
			default:;
			}
		}
	}

	emitter_t emitter;
	emitter_init(&emitter,
	    json ? emitter_output_json_compact : emitter_output_table, write_cb,
	    cbopaque);
	emitter_begin(&emitter);
	emitter_table_printf(&emitter, "___ Begin jemalloc statistics ___\n");
	emitter_json_object_kv_begin(&emitter, "jemalloc");

	if (general) {
		/*
		 * Private (non-public) omission: the per-size-class geometry
		 * arrays are shown only in JSON; the raw format never did.
		 */
		bool omit_size_class_meta = !json;
		stats_general_print(&emitter, omit_size_class_meta);
	}
	if (config_stats) {
		stats_print_runtime_stats(&emitter, merged, destroyed, unmerged,
		    bins, large, mutex, extents, hpa);
	}

	emitter_json_object_end(&emitter); /* Closes the "jemalloc" dict. */
	emitter_table_printf(&emitter, "--- End jemalloc statistics ---\n");
	emitter_end(&emitter);
}

/******************************************************************************/
/* Statistics gathering. */
/*
 * Read stats out of the allocator through mallctl into typed per-section
 * structs.  Keeping the mallctl reads together (and separate from emission)
 * makes coverage auditable.  They are static to src/stats.c; the gather structs
 * and plumbing macros they use live in stats_internal.h.
 */

/* mib must be prepared through "stats.arenas.<i>.extents" (depth 4). */
static void
stats_gather_arena_extent(size_t *mib, unsigned j, stats_arena_extent_t *e) {
	mib[4] = j;
	CTL_LEAF(mib, 5, "ndirty", &e->ndirty, size_t);
	CTL_LEAF(mib, 5, "nmuzzy", &e->nmuzzy, size_t);
	CTL_LEAF(mib, 5, "nretained", &e->nretained, size_t);
	CTL_LEAF(mib, 5, "npinned", &e->npinned, size_t);
	CTL_LEAF(mib, 5, "dirty_bytes", &e->dirty_bytes, size_t);
	CTL_LEAF(mib, 5, "muzzy_bytes", &e->muzzy_bytes, size_t);
	CTL_LEAF(mib, 5, "retained_bytes", &e->retained_bytes, size_t);
	CTL_LEAF(mib, 5, "pinned_bytes", &e->pinned_bytes, size_t);
	e->ntotal = e->ndirty + e->nmuzzy + e->nretained + e->npinned;
	e->total_bytes = e->dirty_bytes + e->muzzy_bytes + e->retained_bytes
	    + e->pinned_bytes;
}

static void
stats_gather_arena_mem(unsigned i, stats_arena_mem_t *mem) {
	CTL_M2_GET("stats.arenas.0.mapped", i, &mem->mapped, size_t);
	CTL_M2_GET("stats.arenas.0.retained", i, &mem->retained, size_t);
	CTL_M2_GET("stats.arenas.0.pinned", i, &mem->pinned, size_t);
	CTL_M2_GET("stats.arenas.0.base", i, &mem->base, size_t);
	CTL_M2_GET("stats.arenas.0.internal", i, &mem->internal, size_t);
	CTL_M2_GET("stats.arenas.0.metadata_edata", i, &mem->metadata_edata,
	    size_t);
	CTL_M2_GET("stats.arenas.0.metadata_rtree", i, &mem->metadata_rtree,
	    size_t);
	CTL_M2_GET("stats.arenas.0.metadata_thp", i, &mem->metadata_thp,
	    size_t);
	CTL_M2_GET("stats.arenas.0.tcache_bytes", i, &mem->tcache_bytes,
	    size_t);
	CTL_M2_GET("stats.arenas.0.tcache_stashed_bytes", i,
	    &mem->tcache_stashed_bytes, size_t);
	CTL_M2_GET("stats.arenas.0.resident", i, &mem->resident, size_t);
	CTL_M2_GET("stats.arenas.0.abandoned_vm", i, &mem->abandoned_vm,
	    size_t);
	CTL_M2_GET("stats.arenas.0.extent_avail", i, &mem->extent_avail,
	    size_t);
	CTL_GET("arenas.page", &mem->page, size_t);
}

static void
stats_gather_arena_basics(unsigned i, stats_arena_basics_t *b) {
	CTL_M2_GET("stats.arenas.0.nthreads", i, &b->nthreads, unsigned);
	CTL_M2_GET("stats.arenas.0.uptime", i, &b->uptime, uint64_t);
	CTL_M2_GET("stats.arenas.0.dss", i, &b->dss, const char *);
	/*
	 * The arena.<i>.name ctl copies the name into the caller-provided
	 * buffer (b->name), so we hand it a real buffer rather than reading
	 * into a borrowed pointer.  The merged/destroyed pseudo-arenas have no
	 * name, and querying them would fail, so guard the read.
	 */
	b->has_name = (i != MALLCTL_ARENAS_ALL && i != MALLCTL_ARENAS_DESTROYED);
	if (b->has_name) {
		char *namep = b->name;
		CTL_M1_GET("arena.0.name", i, (void *)&namep, const char *);
	}
}

static void
stats_gather_arena_decay(unsigned i, stats_arena_decay_t *d) {
	CTL_M2_GET(
	    "stats.arenas.0.dirty_decay_ms", i, &d->dirty_decay_ms, ssize_t);
	CTL_M2_GET(
	    "stats.arenas.0.muzzy_decay_ms", i, &d->muzzy_decay_ms, ssize_t);
	CTL_M2_GET("stats.arenas.0.pactive", i, &d->pactive, size_t);
	CTL_M2_GET("stats.arenas.0.pdirty", i, &d->pdirty, size_t);
	CTL_M2_GET("stats.arenas.0.pmuzzy", i, &d->pmuzzy, size_t);
	CTL_M2_GET("stats.arenas.0.dirty_npurge", i, &d->dirty_npurge, uint64_t);
	CTL_M2_GET(
	    "stats.arenas.0.dirty_nmadvise", i, &d->dirty_nmadvise, uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_purged", i, &d->dirty_purged, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_npurge", i, &d->muzzy_npurge, uint64_t);
	CTL_M2_GET(
	    "stats.arenas.0.muzzy_nmadvise", i, &d->muzzy_nmadvise, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_purged", i, &d->muzzy_purged, uint64_t);
}

static void
stats_gather_arena_alloc(unsigned i, stats_arena_alloc_t *small,
    stats_arena_alloc_t *large) {
	CTL_M2_GET(
	    "stats.arenas.0.small.allocated", i, &small->allocated, size_t);
	CTL_M2_GET("stats.arenas.0.small.nmalloc", i, &small->nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.ndalloc", i, &small->ndalloc, uint64_t);
	CTL_M2_GET(
	    "stats.arenas.0.small.nrequests", i, &small->nrequests, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.nfills", i, &small->nfills, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.nflushes", i, &small->nflushes,
	    uint64_t);
	CTL_M2_GET(
	    "stats.arenas.0.large.allocated", i, &large->allocated, size_t);
	CTL_M2_GET("stats.arenas.0.large.nmalloc", i, &large->nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.ndalloc", i, &large->ndalloc, uint64_t);
	CTL_M2_GET(
	    "stats.arenas.0.large.nrequests", i, &large->nrequests, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.nfills", i, &large->nfills, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.nflushes", i, &large->nflushes,
	    uint64_t);
}

/*
 * nslabs + prof (which gate the table gap/skip) are read by the caller; this
 * reads the remaining detail fields.  mibs prepared + indexed by the caller.
 */
static void
stats_gather_arena_bin(size_t *stats_mib, size_t *arenas_bin_mib,
    stats_arena_bin_t *bin) {
	CTL_LEAF(arenas_bin_mib, 3, "size", &bin->reg_size, size_t);
	CTL_LEAF(arenas_bin_mib, 3, "nregs", &bin->nregs, uint32_t);
	CTL_LEAF(arenas_bin_mib, 3, "slab_size", &bin->slab_size, size_t);
	CTL_LEAF(arenas_bin_mib, 3, "nshards", &bin->nshards, uint32_t);
	CTL_LEAF(stats_mib, 5, "nmalloc", &bin->nmalloc, uint64_t);
	CTL_LEAF(stats_mib, 5, "ndalloc", &bin->ndalloc, uint64_t);
	CTL_LEAF(stats_mib, 5, "curregs", &bin->curregs, size_t);
	CTL_LEAF(stats_mib, 5, "nrequests", &bin->nrequests, uint64_t);
	CTL_LEAF(stats_mib, 5, "nfills", &bin->nfills, uint64_t);
	CTL_LEAF(stats_mib, 5, "nflushes", &bin->nflushes, uint64_t);
	CTL_LEAF(stats_mib, 5, "nreslabs", &bin->nreslabs, uint64_t);
	CTL_LEAF(stats_mib, 5, "curslabs", &bin->curslabs, size_t);
	CTL_LEAF(stats_mib, 5, "nonfull_slabs", &bin->nonfull_slabs, size_t);
}

static void
stats_gather_arena_lextent(size_t *stats_mib, size_t *arenas_lextent_mib,
    size_t *prof_stats_mib, unsigned j, bool prof_stats_on,
    stats_arena_lextent_t *lext) {
	stats_mib[4] = j;
	arenas_lextent_mib[2] = j;
	CTL_LEAF(stats_mib, 5, "nmalloc", &lext->nmalloc, uint64_t);
	CTL_LEAF(stats_mib, 5, "ndalloc", &lext->ndalloc, uint64_t);
	CTL_LEAF(stats_mib, 5, "nrequests", &lext->nrequests, uint64_t);
	CTL_LEAF(arenas_lextent_mib, 3, "size", &lext->lextent_size, size_t);
	CTL_LEAF(stats_mib, 5, "curlextents", &lext->curlextents, size_t);
	if (prof_stats_on) {
		prof_stats_mib[3] = j;
		CTL_LEAF(prof_stats_mib, 4, "live", &lext->prof_live,
		    prof_stats_t);
		CTL_LEAF(prof_stats_mib, 4, "accum", &lext->prof_accum,
		    prof_stats_t);
	}
}

static void
stats_gather_arena_hpa_sec(unsigned i, stats_arena_hpa_sec_t *sec) {
	CTL_M2_GET("stats.arenas.0.hpa_sec_bytes", i, &sec->sec_bytes, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_sec_hits", i, &sec->sec_hits, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_sec_misses", i, &sec->sec_misses, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_sec_dalloc_noflush", i,
	    &sec->sec_dalloc_noflush, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_sec_dalloc_flush", i,
	    &sec->sec_dalloc_flush, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_sec_overfills", i, &sec->sec_overfills,
	    size_t);
}

static void
stats_gather_arena_pac_sec(unsigned i, stats_arena_pac_sec_t *sec) {
	CTL_M2_GET("stats.arenas.0.pac_sec_bytes", i, &sec->sec_bytes, size_t);
	CTL_M2_GET("stats.arenas.0.pac_sec_hits", i, &sec->sec_hits, size_t);
	CTL_M2_GET("stats.arenas.0.pac_sec_misses", i, &sec->sec_misses, size_t);
	CTL_M2_GET("stats.arenas.0.pac_sec_dalloc_noflush", i,
	    &sec->sec_dalloc_noflush, size_t);
	CTL_M2_GET("stats.arenas.0.pac_sec_dalloc_flush", i,
	    &sec->sec_dalloc_flush, size_t);
}

static void
stats_gather_arena_hpa_counters(unsigned i, stats_arena_hpa_counters_t *c) {
	CTL_M2_GET(
	    "stats.arenas.0.hpa_shard.npageslabs", i, &c->npageslabs, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.nactive", i, &c->nactive, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.ndirty", i, &c->ndirty, size_t);

	CTL_M2_GET("stats.arenas.0.hpa_shard.slabs.npageslabs_nonhuge", i,
	    &c->npageslabs_nonhuge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.slabs.nactive_nonhuge", i,
	    &c->nactive_nonhuge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.slabs.ndirty_nonhuge", i,
	    &c->ndirty_nonhuge, size_t);
	c->nretained_nonhuge = c->npageslabs_nonhuge * HUGEPAGE_PAGES
	    - c->nactive_nonhuge - c->ndirty_nonhuge;

	CTL_M2_GET("stats.arenas.0.hpa_shard.slabs.npageslabs_huge", i,
	    &c->npageslabs_huge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.slabs.nactive_huge", i,
	    &c->nactive_huge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.slabs.ndirty_huge", i,
	    &c->ndirty_huge, size_t);

	CTL_M2_GET("stats.arenas.0.hpa_shard.npurge_passes", i,
	    &c->npurge_passes, uint64_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.npurges", i, &c->npurges, uint64_t);
	CTL_M2_GET(
	    "stats.arenas.0.hpa_shard.nhugifies", i, &c->nhugifies, uint64_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.nhugify_failures", i,
	    &c->nhugify_failures, uint64_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.ndehugifies", i, &c->ndehugifies,
	    uint64_t);
}

/* kind is "full_slabs" or "empty_slabs". */
static void
stats_gather_arena_hpa_slab(unsigned i, const char *kind,
    stats_arena_hpa_slab_t *s) {
	size_t mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(mib, 0, "stats.arenas");
	mib[2] = i;
	CTL_LEAF_PREPARE(mib, 3, "hpa_shard");
	CTL_LEAF_PREPARE(mib, 4, kind);
	CTL_LEAF(mib, 5, "npageslabs_huge", &s->npageslabs_huge, size_t);
	CTL_LEAF(mib, 5, "nactive_huge", &s->nactive_huge, size_t);
	CTL_LEAF(mib, 5, "ndirty_huge", &s->ndirty_huge, size_t);
	CTL_LEAF(mib, 5, "npageslabs_nonhuge", &s->npageslabs_nonhuge, size_t);
	CTL_LEAF(mib, 5, "nactive_nonhuge", &s->nactive_nonhuge, size_t);
	CTL_LEAF(mib, 5, "ndirty_nonhuge", &s->ndirty_nonhuge, size_t);
	s->nretained_nonhuge = s->npageslabs_nonhuge * HUGEPAGE_PAGES
	    - s->nactive_nonhuge - s->ndirty_nonhuge;
}

/* mib prepared through "stats.arenas.<i>.hpa_shard.nonfull_slabs" (depth 5). */
static void
stats_gather_arena_hpa_nonfull(size_t *mib, unsigned j,
    stats_arena_hpa_slab_t *s) {
	mib[5] = j;
	CTL_LEAF(mib, 6, "npageslabs_huge", &s->npageslabs_huge, size_t);
	CTL_LEAF(mib, 6, "nactive_huge", &s->nactive_huge, size_t);
	CTL_LEAF(mib, 6, "ndirty_huge", &s->ndirty_huge, size_t);
	CTL_LEAF(mib, 6, "npageslabs_nonhuge", &s->npageslabs_nonhuge, size_t);
	CTL_LEAF(mib, 6, "nactive_nonhuge", &s->nactive_nonhuge, size_t);
	CTL_LEAF(mib, 6, "ndirty_nonhuge", &s->ndirty_nonhuge, size_t);
	s->nretained_nonhuge = s->npageslabs_nonhuge * HUGEPAGE_PAGES
	    - s->nactive_nonhuge - s->ndirty_nonhuge;
}

static void
stats_gather_arena_config(stats_arena_config_t *cfg) {
	CTL_GET("arenas.narenas", &cfg->narenas, unsigned);
	CTL_GET("arenas.dirty_decay_ms", &cfg->dirty_decay_ms, ssize_t);
	CTL_GET("arenas.muzzy_decay_ms", &cfg->muzzy_decay_ms, ssize_t);
	CTL_GET("arenas.quantum", &cfg->quantum, size_t);
	CTL_GET("arenas.page", &cfg->page, size_t);
	CTL_GET("arenas.hugepage", &cfg->hugepage, size_t);
	size_t sz = sizeof(size_t);
	cfg->have_tcache_max = (je_mallctl("arenas.tcache_max",
	    (void *)&cfg->tcache_max, &sz, NULL, 0) == 0);
	CTL_GET("arenas.nbins", &cfg->nbins, unsigned);
	CTL_GET("arenas.nhbins", &cfg->nhbins, unsigned);
	CTL_GET("arenas.nlextents", &cfg->nlextents, unsigned);
}

/* mib must be prepared through "arenas.bin" (depth 2). */
static void
stats_gather_arena_bin_meta(size_t *mib, unsigned j,
    stats_arena_bin_meta_t *bm) {
	mib[2] = j;
	CTL_LEAF(mib, 3, "size", &bm->size, size_t);
	CTL_LEAF(mib, 3, "nregs", &bm->nregs, uint32_t);
	CTL_LEAF(mib, 3, "slab_size", &bm->slab_size, size_t);
	CTL_LEAF(mib, 3, "nshards", &bm->nshards, uint32_t);
}

/* mib must be prepared through "arenas.lextent" (depth 2). */
static void
stats_gather_arena_lextent_meta(size_t *mib, unsigned j,
    stats_arena_lextent_meta_t *lm) {
	mib[2] = j;
	CTL_LEAF(mib, 3, "size", &lm->size, size_t);
}

static void
stats_gather_global(stats_global_t *g) {
	CTL_GET("stats.allocated", &g->allocated, size_t);
	CTL_GET("stats.active", &g->active, size_t);
	CTL_GET("stats.metadata", &g->metadata, size_t);
	CTL_GET("stats.metadata_edata", &g->metadata_edata, size_t);
	CTL_GET("stats.metadata_rtree", &g->metadata_rtree, size_t);
	CTL_GET("stats.metadata_thp", &g->metadata_thp, size_t);
	CTL_GET("stats.resident", &g->resident, size_t);
	CTL_GET("stats.mapped", &g->mapped, size_t);
	CTL_GET("stats.retained", &g->retained, size_t);
	CTL_GET("stats.pinned", &g->pinned, size_t);
	CTL_GET("stats.zero_reallocs", &g->zero_reallocs, size_t);

	if (have_background_thread) {
		CTL_GET("stats.background_thread.num_threads",
		    &g->num_background_threads, size_t);
		CTL_GET("stats.background_thread.num_runs",
		    &g->background_thread_num_runs, uint64_t);
		CTL_GET("stats.background_thread.run_interval",
		    &g->background_thread_run_interval, uint64_t);
	} else {
		g->num_background_threads = 0;
		g->background_thread_num_runs = 0;
		g->background_thread_run_interval = 0;
	}
}

/*
 * Fills initialized[narenas] and *destroyed_initialized; returns the number of
 * initialized arenas.
 */
static unsigned
stats_gather_arenas_initialized(unsigned narenas, bool *initialized,
    bool *destroyed_initialized) {
	size_t mib[3];
	size_t miblen = sizeof(mib) / sizeof(size_t);
	size_t sz;
	unsigned ninitialized = 0;

	xmallctlnametomib("arena.0.initialized", mib, &miblen);
	for (unsigned i = 0; i < narenas; i++) {
		mib[1] = i;
		sz = sizeof(bool);
		xmallctlbymib(mib, miblen, &initialized[i], &sz, NULL, 0);
		if (initialized[i]) {
			ninitialized++;
		}
	}
	mib[1] = MALLCTL_ARENAS_DESTROYED;
	sz = sizeof(bool);
	xmallctlbymib(mib, miblen, destroyed_initialized, &sz, NULL, 0);
	return ninitialized;
}

/******************************************************************************/

static uint64_t
rate_per_second(uint64_t value, uint64_t uptime_ns) {
	uint64_t billion = 1000000000;
	if (uptime_ns == 0 || value == 0) {
		return 0;
	}
	if (uptime_ns < billion) {
		return value;
	} else {
		uint64_t uptime_s = uptime_ns / billion;
		return value / uptime_s;
	}
}

/* Calculate x.yyy and output a string (takes a fixed sized char array). */
static bool
get_rate_str(uint64_t dividend, uint64_t divisor, char str[6]) {
	if (divisor == 0 || dividend > divisor) {
		/* The rate is not supposed to be greater than 1. */
		return true;
	}
	if (dividend > 0) {
		assert(UINT64_MAX / dividend >= 1000);
	}

	unsigned n = (unsigned)((dividend * 1000) / divisor);
	if (n < 10) {
		malloc_snprintf(str, 6, "0.00%u", n);
	} else if (n < 100) {
		malloc_snprintf(str, 6, "0.0%u", n);
	} else if (n < 1000) {
		malloc_snprintf(str, 6, "0.%u", n);
	} else {
		malloc_snprintf(str, 6, "1");
	}

	return false;
}

static void
mutex_stats_init_cols(emitter_row_t *row, const char *table_name,
    emitter_col_t *name,
    emitter_col_t  col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t  col_uint32_t[mutex_prof_num_uint32_t_counters]) {
	mutex_prof_uint64_t_counter_ind_t k_uint64_t = 0;
	mutex_prof_uint32_t_counter_ind_t k_uint32_t = 0;

	emitter_col_t *col;

	if (name != NULL) {
		emitter_col_init(name, row);
		name->justify = emitter_justify_left;
		name->width = 21;
		name->type = emitter_type_title;
		name->str_val = table_name;
	}

#define WIDTH_uint32_t 12
#define WIDTH_uint64_t 16
#define OP(counter, counter_type, human, derived, base_counter)                \
	col = &col_##counter_type[k_##counter_type];                           \
	++k_##counter_type;                                                    \
	emitter_col_init(col, row);                                            \
	col->justify = emitter_justify_right;                                  \
	col->width = derived ? 8 : WIDTH_##counter_type;                       \
	col->type = emitter_type_title;                                        \
	col->str_val = human;
	MUTEX_PROF_COUNTERS
#undef OP
#undef WIDTH_uint32_t
#undef WIDTH_uint64_t
	col_uint64_t[mutex_counter_total_wait_time_ps].width = 10;
}

/*
 * Read one mutex's counters (leaf "name" under the prepared mib) into the
 * emitter columns.  Shared by the global and per-arena mutex tables, whose rows
 * differ only in the mib and mutex name they pass.
 */
static void
mutex_stats_read(size_t mib[], size_t miblen, const char *name,
    emitter_col_t *col_name,
    emitter_col_t  col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t  col_uint32_t[mutex_prof_num_uint32_t_counters],
    uint64_t       uptime) {
	CTL_LEAF_PREPARE(mib, miblen, name);
	size_t miblen_name = miblen + 1;

	col_name->str_val = name;

	emitter_col_t *dst;
#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, counter_type, human, derived, base_counter)                \
	dst = &col_##counter_type[mutex_counter_##counter];                    \
	dst->type = EMITTER_TYPE_##counter_type;                               \
	if (!derived) {                                                        \
		CTL_LEAF(mib, miblen_name, #counter,                           \
		    (counter_type *)&dst->bool_val, counter_type);             \
	} else {                                                               \
		emitter_col_t *base =                                          \
		    &col_##counter_type[mutex_counter_##base_counter];         \
		dst->counter_type##_val = (counter_type)rate_per_second(       \
		    base->counter_type##_val, uptime);                         \
	}
	MUTEX_PROF_COUNTERS
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

static void
mutex_stats_read_arena_bin(size_t mib[], size_t miblen,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters],
    uint64_t      uptime) {
	CTL_LEAF_PREPARE(mib, miblen, "mutex");
	size_t miblen_mutex = miblen + 1;

	emitter_col_t *dst;

#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, counter_type, human, derived, base_counter)                \
	dst = &col_##counter_type[mutex_counter_##counter];                    \
	dst->type = EMITTER_TYPE_##counter_type;                               \
	if (!derived) {                                                        \
		CTL_LEAF(mib, miblen_mutex, #counter,                          \
		    (counter_type *)&dst->bool_val, counter_type);             \
	} else {                                                               \
		emitter_col_t *base =                                          \
		    &col_##counter_type[mutex_counter_##base_counter];         \
		dst->counter_type##_val = (counter_type)rate_per_second(       \
		    base->counter_type##_val, uptime);                         \
	}
	MUTEX_PROF_COUNTERS
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

/* "row" can be NULL to avoid emitting in table mode. */
static void
mutex_stats_emit(emitter_t *emitter, emitter_row_t *row,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters]) {
	if (row != NULL) {
		emitter_table_row(emitter, row);
	}

	mutex_prof_uint64_t_counter_ind_t k_uint64_t = 0;
	mutex_prof_uint32_t_counter_ind_t k_uint32_t = 0;

	emitter_col_t *col;

#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, type, human, derived, base_counter)                        \
	if (!derived) {                                                        \
		col = &col_##type[k_##type];                                   \
		emitter_json_kv(emitter, #counter, EMITTER_TYPE_##type,        \
		    (const void *)&col->bool_val);                             \
	}                                                                      \
	++k_##type;
	MUTEX_PROF_COUNTERS;
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

/*
 * Set a per-size-class row's "size" column.  Above the slow-growth threshold,
 * display the bucket bounds as "(prev_size,size]".  This notation does not
 * imply that every integer in the interval is a valid usable size.  Smaller
 * classes are displayed as a single size.  buf must outlive row emission.
 * (This is table labeling only; the ordered JSON arrays imply the bounds.)
 */
static void
stats_size_col_set(emitter_col_t *col, size_t size, size_t prev_size, char *buf,
    size_t bufsz) {
	if (size > USIZE_GROW_SLOW_THRESHOLD) {
		malloc_snprintf(buf, bufsz, "(%zu,%zu]", prev_size, size);
		col->type = emitter_type_title;
		col->str_val = buf;
	} else {
		col->type = emitter_type_size;
		col->size_val = size;
	}
}

enum {
	STATS_COL_FLAG_NONE = 0,
	STATS_COL_FLAG_PROF = 1U << 0,
};

static unsigned
stats_col_active_flags(bool prof_stats_on) {
	return prof_stats_on ? STATS_COL_FLAG_PROF : STATS_COL_FLAG_NONE;
}

typedef struct {
	const stats_arena_bin_t *bin;
	unsigned                 ind;
	size_t                   page;
	uint64_t                 uptime;
	const char              *util;
} stats_arena_bin_emit_row_t;

#define BIN_COL_GET(name, value_member, field)                                \
	static void                                                            \
	stats_bin_col_get_##name(const void *vrow, emitter_col_t *col) {        \
		const stats_arena_bin_emit_row_t *row = vrow;                    \
		col->value_member = row->bin->field;                              \
	}
#define BIN_COL_GET_RATE(name, field)                                         \
	static void                                                            \
	stats_bin_col_get_##name(const void *vrow, emitter_col_t *col) {        \
		const stats_arena_bin_emit_row_t *row = vrow;                    \
		col->uint64_val = rate_per_second(                               \
		    row->bin->field, row->uptime);                                \
	}
BIN_COL_GET(size, size_val, reg_size)
BIN_COL_GET(nmalloc, uint64_val, nmalloc)
BIN_COL_GET(ndalloc, uint64_val, ndalloc)
BIN_COL_GET(nrequests, uint64_val, nrequests)
BIN_COL_GET(prof_live_requested, uint64_val, prof_live.req_sum)
BIN_COL_GET(prof_live_count, uint64_val, prof_live.count)
BIN_COL_GET(prof_accum_requested, uint64_val, prof_accum.req_sum)
BIN_COL_GET(prof_accum_count, uint64_val, prof_accum.count)
BIN_COL_GET(nshards, unsigned_val, nshards)
BIN_COL_GET(curregs, size_val, curregs)
BIN_COL_GET(curslabs, size_val, curslabs)
BIN_COL_GET(nonfull_slabs, size_val, nonfull_slabs)
BIN_COL_GET(regs, unsigned_val, nregs)
BIN_COL_GET(nfills, uint64_val, nfills)
BIN_COL_GET(nflushes, uint64_val, nflushes)
BIN_COL_GET(nslabs, uint64_val, nslabs)
BIN_COL_GET(nreslabs, uint64_val, nreslabs)
BIN_COL_GET_RATE(nmalloc_ps, nmalloc)
BIN_COL_GET_RATE(ndalloc_ps, ndalloc)
BIN_COL_GET_RATE(nrequests_ps, nrequests)
BIN_COL_GET_RATE(nfills_ps, nfills)
BIN_COL_GET_RATE(nflushes_ps, nflushes)
BIN_COL_GET_RATE(nreslabs_ps, nreslabs)
#undef BIN_COL_GET
#undef BIN_COL_GET_RATE

static void
stats_bin_col_get_ind(const void *vrow, emitter_col_t *col) {
	const stats_arena_bin_emit_row_t *row = vrow;
	col->unsigned_val = row->ind;
}

static void
stats_bin_col_get_allocated(const void *vrow, emitter_col_t *col) {
	const stats_arena_bin_emit_row_t *row = vrow;
	col->size_val = row->bin->curregs * row->bin->reg_size;
}

static void
stats_bin_col_get_pgs(const void *vrow, emitter_col_t *col) {
	const stats_arena_bin_emit_row_t *row = vrow;
	col->size_val = row->bin->slab_size / row->page;
}

static void
stats_bin_col_get_spacer(const void *vrow, emitter_col_t *col) {
	(void)vrow;
	col->str_val = " ";
}

static void
stats_bin_col_get_util(const void *vrow, emitter_col_t *col) {
	const stats_arena_bin_emit_row_t *row = vrow;
	col->str_val = row->util;
}

enum {
	BIN_COL_SIZE,
	BIN_COL_IND,
	BIN_COL_ALLOCATED,
	BIN_COL_NMALLOC,
	BIN_COL_NMALLOC_PS,
	BIN_COL_NDALLOC,
	BIN_COL_NDALLOC_PS,
	BIN_COL_NREQUESTS,
	BIN_COL_NREQUESTS_PS,
	BIN_COL_PROF_LIVE_REQUESTED,
	BIN_COL_PROF_LIVE_COUNT,
	BIN_COL_PROF_ACCUM_REQUESTED,
	BIN_COL_PROF_ACCUM_COUNT,
	BIN_COL_NSHARDS,
	BIN_COL_CURREGS,
	BIN_COL_CURSLABS,
	BIN_COL_NONFULL_SLABS,
	BIN_COL_REGS,
	BIN_COL_PGS,
	BIN_COL_SPACER,
	BIN_COL_UTIL,
	BIN_COL_NFILLS,
	BIN_COL_NFILLS_PS,
	BIN_COL_NFLUSHES,
	BIN_COL_NFLUSHES_PS,
	BIN_COL_NSLABS,
	BIN_COL_NRESLABS,
	BIN_COL_NRESLABS_PS,
	BIN_COL_COUNT
};

#define BIN_DESC(key, label, width, type, flags, name)                        \
	{key, label, emitter_justify_right, width, emitter_type_##type, flags,   \
	    stats_bin_col_get_##name}
static const emitter_col_desc_t stats_bin_cols[] = {
	BIN_DESC(NULL, "size", 20, size, STATS_COL_FLAG_NONE, size),
	BIN_DESC(NULL, "ind", 4, unsigned, STATS_COL_FLAG_NONE, ind),
	BIN_DESC(NULL, "allocated", 14, size, STATS_COL_FLAG_NONE, allocated),
	BIN_DESC("nmalloc", "nmalloc", 14, uint64, STATS_COL_FLAG_NONE,
	    nmalloc),
	BIN_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE, nmalloc_ps),
	BIN_DESC("ndalloc", "ndalloc", 14, uint64, STATS_COL_FLAG_NONE,
	    ndalloc),
	BIN_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE, ndalloc_ps),
	BIN_DESC("nrequests", "nrequests", 15, uint64, STATS_COL_FLAG_NONE,
	    nrequests),
	BIN_DESC(NULL, "(#/sec)", 10, uint64, STATS_COL_FLAG_NONE,
	    nrequests_ps),
	BIN_DESC("prof_live_requested", "prof_live_requested", 21, uint64,
	    STATS_COL_FLAG_PROF, prof_live_requested),
	BIN_DESC("prof_live_count", "prof_live_count", 17, uint64,
	    STATS_COL_FLAG_PROF, prof_live_count),
	BIN_DESC("prof_accum_requested", "prof_accum_requested", 21, uint64,
	    STATS_COL_FLAG_PROF, prof_accum_requested),
	BIN_DESC("prof_accum_count", "prof_accum_count", 17, uint64,
	    STATS_COL_FLAG_PROF, prof_accum_count),
	BIN_DESC(NULL, "nshards", 9, unsigned, STATS_COL_FLAG_NONE, nshards),
	BIN_DESC("curregs", "curregs", 13, size, STATS_COL_FLAG_NONE, curregs),
	BIN_DESC("curslabs", "curslabs", 13, size, STATS_COL_FLAG_NONE,
	    curslabs),
	BIN_DESC("nonfull_slabs", "nonfull_slabs", 15, size,
	    STATS_COL_FLAG_NONE,
	    nonfull_slabs),
	BIN_DESC(NULL, "regs", 5, unsigned, STATS_COL_FLAG_NONE, regs),
	BIN_DESC(NULL, "pgs", 4, size, STATS_COL_FLAG_NONE, pgs),
	BIN_DESC(NULL, " ", 1, title, STATS_COL_FLAG_NONE, spacer),
	BIN_DESC(NULL, "util", 6, title, STATS_COL_FLAG_NONE, util),
	BIN_DESC("nfills", "nfills", 13, uint64, STATS_COL_FLAG_NONE, nfills),
	BIN_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE, nfills_ps),
	BIN_DESC("nflushes", "nflushes", 13, uint64, STATS_COL_FLAG_NONE,
	    nflushes),
	BIN_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE,
	    nflushes_ps),
	BIN_DESC(NULL, "nslabs", 13, uint64, STATS_COL_FLAG_NONE, nslabs),
	BIN_DESC("nreslabs", "nreslabs", 13, uint64, STATS_COL_FLAG_NONE,
	    nreslabs),
	BIN_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE,
	    nreslabs_ps),
};
#undef BIN_DESC
_Static_assert(sizeof(stats_bin_cols) / sizeof(stats_bin_cols[0]) ==
    BIN_COL_COUNT, "stats_bin_cols must match BIN_COL_COUNT");

static const unsigned stats_bin_json_order[] = {
	BIN_COL_NMALLOC,
	BIN_COL_NDALLOC,
	BIN_COL_CURREGS,
	BIN_COL_NREQUESTS,
	BIN_COL_PROF_LIVE_REQUESTED,
	BIN_COL_PROF_LIVE_COUNT,
	BIN_COL_PROF_ACCUM_REQUESTED,
	BIN_COL_PROF_ACCUM_COUNT,
	BIN_COL_NFILLS,
	BIN_COL_NFLUSHES,
	BIN_COL_NRESLABS,
	BIN_COL_CURSLABS,
	BIN_COL_NONFULL_SLABS,
};

static void
stats_emit_arena_bin_row(emitter_t *emitter, emitter_row_t *table_row,
    emitter_col_t *cols, const stats_arena_bin_emit_row_t *row,
    bool prof_stats_on, bool mutex, emitter_col_t *mutex64,
    emitter_col_t *mutex32, bool is_gap) {
	unsigned active_flags = stats_col_active_flags(prof_stats_on);
	emitter_col_table_fill(
	    stats_bin_cols, BIN_COL_COUNT, active_flags, cols, row);
	emitter_json_object_begin(emitter);
	emitter_col_table_emit_json(emitter, stats_bin_cols, BIN_COL_COUNT,
	    active_flags, cols, stats_bin_json_order,
	    sizeof(stats_bin_json_order) / sizeof(stats_bin_json_order[0]));
	if (mutex) {
		emitter_json_object_kv_begin(emitter, "mutex");
		mutex_stats_emit(emitter, NULL, mutex64, mutex32);
		emitter_json_object_end(emitter);
	}
	emitter_json_object_end(emitter);
	emitter_table_sparse_row(emitter, table_row, is_gap);
}

JEMALLOC_COLD
static void
stats_arena_bins_print(
    emitter_t *emitter, bool mutex, unsigned i, uint64_t uptime) {
	/*
	 * Process one size-class row at a time to preserve O(1) memory.  Full
	 * gather-all-rows-before-emit separation would require buffering the
	 * table, so gathering and emission remain interleaved across rows.  The
	 * lextents, extents, and HPA nonfull tables follow the same pattern.
	 */
	size_t   page;
	unsigned nbins, j;

	CTL_GET("arenas.page", &page, size_t);

	CTL_GET("arenas.nbins", &nbins, unsigned);

	bool prof_stats_on = config_prof && opt_prof && opt_prof_stats
	    && i == MALLCTL_ARENAS_ALL;
	emitter_row_t row, header_row;
	emitter_col_t cols[BIN_COL_COUNT], header_cols[BIN_COL_COUNT];
	emitter_col_table_build(stats_bin_cols, BIN_COL_COUNT,
	    stats_col_active_flags(prof_stats_on), &row, cols, &header_row,
	    header_cols);

	emitter_col_t col_mutex64[mutex_prof_num_uint64_t_counters];
	emitter_col_t col_mutex32[mutex_prof_num_uint32_t_counters];

	emitter_col_t header_mutex64[mutex_prof_num_uint64_t_counters];
	emitter_col_t header_mutex32[mutex_prof_num_uint32_t_counters];

	if (mutex) {
		mutex_stats_init_cols(
		    &row, NULL, NULL, col_mutex64, col_mutex32);
		mutex_stats_init_cols(
		    &header_row, NULL, NULL, header_mutex64, header_mutex32);
	}

	emitter_col_table_header(emitter, &header_row,
	    &header_cols[BIN_COL_SIZE], "bins:", "bins");

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "bins");

	size_t arenas_bin_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(arenas_bin_mib, 0, "arenas.bin");

	size_t prof_stats_mib[CTL_MAX_DEPTH];
	if (prof_stats_on) {
		CTL_LEAF_PREPARE(prof_stats_mib, 0, "prof.stats.bins");
	}

	emitter_table_sparse_begin(emitter);
	for (j = 0; j < nbins; j++) {
		stats_arena_bin_t bin;
		stats_arenas_mib[4] = j;
		arenas_bin_mib[2] = j;

		CTL_LEAF(stats_arenas_mib, 5, "nslabs", &bin.nslabs, uint64_t);

		if (prof_stats_on) {
			prof_stats_mib[3] = j;
			CTL_LEAF(prof_stats_mib, 4, "live", &bin.prof_live,
			    prof_stats_t);
			CTL_LEAF(prof_stats_mib, 4, "accum", &bin.prof_accum,
			    prof_stats_t);
		}

		bool is_gap;
		if (prof_stats_on) {
			is_gap = (bin.nslabs == 0 && bin.prof_accum.count == 0);
		} else {
			is_gap = (bin.nslabs == 0);
		}

		if (is_gap && !emitter_outputs_json(emitter)) {
			emitter_table_sparse_row(emitter, &row, true);
			continue;
		}

		/* Mutex counters still gather directly into their emitter columns. */
		stats_gather_arena_bin(stats_arenas_mib, arenas_bin_mib, &bin);

		if (mutex) {
			mutex_stats_read_arena_bin(stats_arenas_mib, 5,
			    col_mutex64, col_mutex32, uptime);
		}

		size_t availregs = bin.nregs * bin.curslabs;
		char   util[6];
		if (get_rate_str(
		        (uint64_t)bin.curregs, (uint64_t)availregs, util)) {
			if (availregs == 0) {
				malloc_snprintf(util, sizeof(util), "1");
			} else if (bin.curregs > availregs) {
				/*
				 * Race detected: the counters were read in
				 * separate mallctl calls and concurrent
				 * operations happened in between.  In this case
				 * no meaningful utilization can be computed.
				 */
				malloc_snprintf(util, sizeof(util), " race");
			} else {
				not_reached();
			}
		}

		stats_arena_bin_emit_row_t emit_row = {
		    &bin, j, page, uptime, util};
		stats_emit_arena_bin_row(emitter, &row, cols, &emit_row,
		    prof_stats_on, mutex, col_mutex64, col_mutex32, is_gap);
	}
	emitter_json_array_end(emitter); /* Close "bins". */

	emitter_table_sparse_end(emitter);
}

typedef struct {
	const stats_arena_lextent_t *lextent;
	unsigned                     ind;
	uint64_t                     uptime;
} stats_arena_lextent_emit_row_t;

#define LEXTENT_COL_GET(name, value_member, field)                            \
	static void                                                            \
	stats_lextent_col_get_##name(const void *vrow, emitter_col_t *col) {    \
		const stats_arena_lextent_emit_row_t *row = vrow;                \
		col->value_member = row->lextent->field;                          \
	}
#define LEXTENT_COL_GET_RATE(name, field)                                     \
	static void                                                            \
	stats_lextent_col_get_##name(const void *vrow, emitter_col_t *col) {    \
		const stats_arena_lextent_emit_row_t *row = vrow;                \
		col->uint64_val = rate_per_second(                               \
		    row->lextent->field, row->uptime);                            \
	}
LEXTENT_COL_GET(size, size_val, lextent_size)
LEXTENT_COL_GET(nmalloc, uint64_val, nmalloc)
LEXTENT_COL_GET(ndalloc, uint64_val, ndalloc)
LEXTENT_COL_GET(nrequests, uint64_val, nrequests)
LEXTENT_COL_GET(prof_live_requested, uint64_val, prof_live.req_sum)
LEXTENT_COL_GET(prof_live_count, uint64_val, prof_live.count)
LEXTENT_COL_GET(prof_accum_requested, uint64_val, prof_accum.req_sum)
LEXTENT_COL_GET(prof_accum_count, uint64_val, prof_accum.count)
LEXTENT_COL_GET(curlextents, size_val, curlextents)
LEXTENT_COL_GET_RATE(nmalloc_ps, nmalloc)
LEXTENT_COL_GET_RATE(ndalloc_ps, ndalloc)
LEXTENT_COL_GET_RATE(nrequests_ps, nrequests)
#undef LEXTENT_COL_GET
#undef LEXTENT_COL_GET_RATE

static void
stats_lextent_col_get_ind(const void *vrow, emitter_col_t *col) {
	const stats_arena_lextent_emit_row_t *row = vrow;
	col->unsigned_val = row->ind;
}

static void
stats_lextent_col_get_allocated(const void *vrow, emitter_col_t *col) {
	const stats_arena_lextent_emit_row_t *row = vrow;
	col->size_val = row->lextent->curlextents * row->lextent->lextent_size;
}

enum {
	LEXTENT_COL_SIZE,
	LEXTENT_COL_IND,
	LEXTENT_COL_ALLOCATED,
	LEXTENT_COL_NMALLOC,
	LEXTENT_COL_NMALLOC_PS,
	LEXTENT_COL_NDALLOC,
	LEXTENT_COL_NDALLOC_PS,
	LEXTENT_COL_NREQUESTS,
	LEXTENT_COL_NREQUESTS_PS,
	LEXTENT_COL_PROF_LIVE_REQUESTED,
	LEXTENT_COL_PROF_LIVE_COUNT,
	LEXTENT_COL_PROF_ACCUM_REQUESTED,
	LEXTENT_COL_PROF_ACCUM_COUNT,
	LEXTENT_COL_CURLEXTENTS,
	LEXTENT_COL_COUNT
};

#define LEXTENT_DESC(key, label, width, type, flags, name)                    \
	{key, label, emitter_justify_right, width, emitter_type_##type, flags,   \
	    stats_lextent_col_get_##name}
static const emitter_col_desc_t stats_lextent_cols[] = {
	LEXTENT_DESC(NULL, "size", 20, size, STATS_COL_FLAG_NONE, size),
	LEXTENT_DESC(NULL, "ind", 4, unsigned, STATS_COL_FLAG_NONE, ind),
	LEXTENT_DESC(NULL, "allocated", 13, size, STATS_COL_FLAG_NONE,
	    allocated),
	LEXTENT_DESC(NULL, "nmalloc", 13, uint64, STATS_COL_FLAG_NONE, nmalloc),
	LEXTENT_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE,
	    nmalloc_ps),
	LEXTENT_DESC(NULL, "ndalloc", 13, uint64, STATS_COL_FLAG_NONE, ndalloc),
	LEXTENT_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE,
	    ndalloc_ps),
	LEXTENT_DESC(NULL, "nrequests", 13, uint64, STATS_COL_FLAG_NONE,
	    nrequests),
	LEXTENT_DESC(NULL, "(#/sec)", 8, uint64, STATS_COL_FLAG_NONE,
	    nrequests_ps),
	LEXTENT_DESC("prof_live_requested", "prof_live_requested", 21, uint64,
	    STATS_COL_FLAG_PROF, prof_live_requested),
	LEXTENT_DESC("prof_live_count", "prof_live_count", 17, uint64,
	    STATS_COL_FLAG_PROF, prof_live_count),
	LEXTENT_DESC("prof_accum_requested", "prof_accum_requested", 21,
	    uint64, STATS_COL_FLAG_PROF, prof_accum_requested),
	LEXTENT_DESC("prof_accum_count", "prof_accum_count", 17, uint64,
	    STATS_COL_FLAG_PROF, prof_accum_count),
	LEXTENT_DESC("curlextents", "curlextents", 13, size,
	    STATS_COL_FLAG_NONE, curlextents),
};
#undef LEXTENT_DESC
_Static_assert(sizeof(stats_lextent_cols) / sizeof(stats_lextent_cols[0]) ==
    LEXTENT_COL_COUNT, "stats_lextent_cols must match LEXTENT_COL_COUNT");

static const unsigned stats_lextent_json_order[] = {
	LEXTENT_COL_PROF_LIVE_REQUESTED,
	LEXTENT_COL_PROF_LIVE_COUNT,
	LEXTENT_COL_PROF_ACCUM_REQUESTED,
	LEXTENT_COL_PROF_ACCUM_COUNT,
	LEXTENT_COL_CURLEXTENTS,
};

static void
stats_emit_arena_lextent_row(emitter_t *emitter, emitter_row_t *table_row,
    emitter_col_t *cols, const stats_arena_lextent_emit_row_t *row,
    bool prof_stats_on, size_t prev_size, char *size_buf,
    size_t size_buf_size, bool is_gap) {
	unsigned active_flags = stats_col_active_flags(prof_stats_on);
	emitter_col_table_fill(stats_lextent_cols, LEXTENT_COL_COUNT,
	    active_flags, cols, row);
	stats_size_col_set(&cols[LEXTENT_COL_SIZE], row->lextent->lextent_size,
	    prev_size, size_buf, size_buf_size);
	emitter_json_object_begin(emitter);
	emitter_col_table_emit_json(emitter, stats_lextent_cols,
	    LEXTENT_COL_COUNT, active_flags, cols, stats_lextent_json_order,
	    sizeof(stats_lextent_json_order) /
	        sizeof(stats_lextent_json_order[0]));
	emitter_table_sparse_row(emitter, table_row, is_gap);
	emitter_json_object_end(emitter);
}

JEMALLOC_COLD
static void
stats_arena_lextents_print(emitter_t *emitter, unsigned i, uint64_t uptime) {
	/* Streaming table; see stats_arena_bins_print for the rationale. */
	unsigned nbins, nlextents, j;

	CTL_GET("arenas.nbins", &nbins, unsigned);
	CTL_GET("arenas.nlextents", &nlextents, unsigned);

	bool prof_stats_on = config_prof && opt_prof && opt_prof_stats
	    && i == MALLCTL_ARENAS_ALL;
	emitter_row_t row, header_row;
	emitter_col_t cols[LEXTENT_COL_COUNT], header_cols[LEXTENT_COL_COUNT];
	char size_buf[48];
	emitter_col_table_build(stats_lextent_cols, LEXTENT_COL_COUNT,
	    stats_col_active_flags(prof_stats_on), &row, cols, &header_row,
	    header_cols);
	emitter_col_table_header(emitter, &header_row,
	    &header_cols[LEXTENT_COL_SIZE], "large:", "lextents");

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "lextents");

	size_t arenas_lextent_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(arenas_lextent_mib, 0, "arenas.lextent");

	size_t prof_stats_mib[CTL_MAX_DEPTH];
	if (prof_stats_on) {
		CTL_LEAF_PREPARE(prof_stats_mib, 0, "prof.stats.lextents");
	}

	/*
	 * Carry the previous size class's size forward for the "(prev,size]"
	 * range label instead of looking it up by a large size-class index:
	 * with large size classes disabled (the default) that index may be out
	 * of range, and indexing sz_index2size_tab there would be unsafe.  The
	 * class before the first large one is the last bin; sz_index2size_compute
	 * derives it arithmetically (no table access).
	 */
	size_t prev_size = sz_index2size_compute(nbins - 1);
	emitter_table_sparse_begin(emitter);
	for (j = 0; j < nlextents; j++) {
		stats_arena_lextent_t lext;
		stats_gather_arena_lextent(stats_arenas_mib, arenas_lextent_mib,
		    prof_stats_mib, j, prof_stats_on, &lext);

		bool is_gap = (lext.nrequests == 0);

		stats_arena_lextent_emit_row_t emit_row = {
		    &lext, nbins + j, uptime};
		stats_emit_arena_lextent_row(emitter, &row, cols, &emit_row,
		    prof_stats_on, prev_size, size_buf, sizeof(size_buf), is_gap);
		prev_size = lext.lextent_size;
	}
	emitter_json_array_end(emitter); /* Close "lextents". */
	emitter_table_sparse_end(emitter);
}

typedef struct {
	const stats_arena_extent_t *extent;
	unsigned                    ind;
	size_t                      size;
} stats_arena_extent_emit_row_t;

#define EXTENT_COL_GET(name, field)                                           \
	static void                                                            \
	stats_extent_col_get_##name(const void *vrow, emitter_col_t *col) {     \
		const stats_arena_extent_emit_row_t *row = vrow;                 \
		col->size_val = row->extent->field;                              \
	}
EXTENT_COL_GET(ndirty, ndirty)
EXTENT_COL_GET(dirty, dirty_bytes)
EXTENT_COL_GET(nmuzzy, nmuzzy)
EXTENT_COL_GET(muzzy, muzzy_bytes)
EXTENT_COL_GET(nretained, nretained)
EXTENT_COL_GET(retained, retained_bytes)
EXTENT_COL_GET(npinned, npinned)
EXTENT_COL_GET(pinned, pinned_bytes)
EXTENT_COL_GET(ntotal, ntotal)
EXTENT_COL_GET(total, total_bytes)
#undef EXTENT_COL_GET

static void
stats_extent_col_get_size(const void *vrow, emitter_col_t *col) {
	const stats_arena_extent_emit_row_t *row = vrow;
	col->size_val = row->size;
}

static void
stats_extent_col_get_ind(const void *vrow, emitter_col_t *col) {
	const stats_arena_extent_emit_row_t *row = vrow;
	col->unsigned_val = row->ind;
}

enum {
	EXTENT_COL_SIZE,
	EXTENT_COL_IND,
	EXTENT_COL_NDIRTY,
	EXTENT_COL_DIRTY,
	EXTENT_COL_NMUZZY,
	EXTENT_COL_MUZZY,
	EXTENT_COL_NRETAINED,
	EXTENT_COL_RETAINED,
	EXTENT_COL_NPINNED,
	EXTENT_COL_PINNED,
	EXTENT_COL_NTOTAL,
	EXTENT_COL_TOTAL,
	EXTENT_COL_COUNT
};

#define EXTENT_DESC(key, label, name)                                         \
	{key, label, emitter_justify_right, 13, emitter_type_size,               \
	    STATS_COL_FLAG_NONE, stats_extent_col_get_##name}
static const emitter_col_desc_t stats_extent_cols[] = {
	{NULL, "size", emitter_justify_right, 20, emitter_type_size,
	    STATS_COL_FLAG_NONE, stats_extent_col_get_size},
	{NULL, "ind", emitter_justify_right, 4, emitter_type_unsigned,
	    STATS_COL_FLAG_NONE, stats_extent_col_get_ind},
	EXTENT_DESC("ndirty", "ndirty", ndirty),
	EXTENT_DESC("dirty_bytes", "dirty", dirty),
	EXTENT_DESC("nmuzzy", "nmuzzy", nmuzzy),
	EXTENT_DESC("muzzy_bytes", "muzzy", muzzy),
	EXTENT_DESC("nretained", "nretained", nretained),
	EXTENT_DESC("retained_bytes", "retained", retained),
	EXTENT_DESC("npinned", "npinned", npinned),
	EXTENT_DESC("pinned_bytes", "pinned", pinned),
	EXTENT_DESC(NULL, "ntotal", ntotal),
	EXTENT_DESC(NULL, "total", total),
};
#undef EXTENT_DESC
_Static_assert(sizeof(stats_extent_cols) / sizeof(stats_extent_cols[0]) ==
    EXTENT_COL_COUNT, "stats_extent_cols must match EXTENT_COL_COUNT");

static const unsigned stats_extent_json_order[] = {
	EXTENT_COL_NDIRTY,
	EXTENT_COL_NMUZZY,
	EXTENT_COL_NRETAINED,
	EXTENT_COL_NPINNED,
	EXTENT_COL_DIRTY,
	EXTENT_COL_MUZZY,
	EXTENT_COL_RETAINED,
	EXTENT_COL_PINNED,
};

static void
stats_emit_arena_extent_row(emitter_t *emitter, emitter_row_t *table_row,
    emitter_col_t *cols, const stats_arena_extent_emit_row_t *row,
    size_t prev_size, char *size_buf, size_t size_buf_size, bool is_gap) {
	emitter_col_table_fill(stats_extent_cols, EXTENT_COL_COUNT,
	    STATS_COL_FLAG_NONE, cols, row);
	stats_size_col_set(&cols[EXTENT_COL_SIZE], row->size, prev_size,
	    size_buf, size_buf_size);
	emitter_json_object_begin(emitter);
	emitter_col_table_emit_json(emitter, stats_extent_cols, EXTENT_COL_COUNT,
	    STATS_COL_FLAG_NONE, cols, stats_extent_json_order,
	    sizeof(stats_extent_json_order) /
	        sizeof(stats_extent_json_order[0]));
	emitter_json_object_end(emitter);
	emitter_table_sparse_row(emitter, table_row, is_gap);
}

JEMALLOC_COLD
static void
stats_arena_extents_print(emitter_t *emitter, unsigned i) {
	/* Streaming table; see stats_arena_bins_print for the rationale. */
	unsigned      j;
	emitter_row_t row, header_row;
	emitter_col_t cols[EXTENT_COL_COUNT], header_cols[EXTENT_COL_COUNT];
	char size_buf[48];
	emitter_col_table_build(stats_extent_cols, EXTENT_COL_COUNT,
	    STATS_COL_FLAG_NONE, &row, cols, &header_row, header_cols);
	emitter_col_table_header(emitter, &header_row,
	    &header_cols[EXTENT_COL_SIZE], "extents:", "extents");

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "extents");

	emitter_table_sparse_begin(emitter);
	for (j = 0; j < SC_NPSIZES; j++) {
		stats_arena_extent_t e;
		stats_gather_arena_extent(stats_arenas_mib, j, &e);

		bool is_gap = (e.ntotal == 0);

		stats_arena_extent_emit_row_t emit_row = {&e, j, sz_pind2sz(j)};
		stats_emit_arena_extent_row(emitter, &row, cols, &emit_row,
		    j > 0 ? sz_pind2sz(j - 1) : 0, size_buf, sizeof(size_buf),
		    is_gap);
	}
	emitter_json_array_end(emitter); /* Close "extents". */
	emitter_table_sparse_end(emitter);
}

static void
stats_emit_arena_hpa_sec(emitter_t *emitter, const stats_arena_hpa_sec_t *sec) {
	emitter_kv(emitter, "sec_bytes", "Bytes in small extent cache",
	    emitter_type_size, &sec->sec_bytes);
	emitter_kv(emitter, "sec_hits", "Total hits in small extent cache",
	    emitter_type_size, &sec->sec_hits);
	emitter_kv(emitter, "sec_misses", "Total misses in small extent cache",
	    emitter_type_size, &sec->sec_misses);
	emitter_kv(emitter, "sec_dalloc_noflush",
	    "Dalloc calls without flush in small extent cache",
	    emitter_type_size, &sec->sec_dalloc_noflush);
	emitter_kv(emitter, "sec_dalloc_flush",
	    "Dalloc calls with flush in small extent cache", emitter_type_size,
	    &sec->sec_dalloc_flush);
	emitter_kv(emitter, "sec_overfills",
	    "sec_fill calls that went over max_bytes", emitter_type_size,
	    &sec->sec_overfills);
}

static void
stats_arena_hpa_shard_sec_print(emitter_t *emitter, unsigned i) {
	stats_arena_hpa_sec_t sec;
	stats_gather_arena_hpa_sec(i, &sec);
	stats_emit_arena_hpa_sec(emitter, &sec);
}

static void
stats_emit_arena_pac_sec(emitter_t *emitter, const stats_arena_pac_sec_t *sec) {
	emitter_kv(emitter, "pac_sec_bytes", "Bytes in PAC small extent cache",
	    emitter_type_size, &sec->sec_bytes);
	emitter_kv(emitter, "pac_sec_hits", "Total hits in PAC small extent cache",
	    emitter_type_size, &sec->sec_hits);
	emitter_kv(emitter, "pac_sec_misses",
	    "Total misses in PAC small extent cache", emitter_type_size,
	    &sec->sec_misses);
	emitter_kv(emitter, "pac_sec_dalloc_noflush",
	    "Dalloc calls without flush in PAC small extent cache",
	    emitter_type_size, &sec->sec_dalloc_noflush);
	emitter_kv(emitter, "pac_sec_dalloc_flush",
	    "Dalloc calls with flush in PAC small extent cache",
	    emitter_type_size, &sec->sec_dalloc_flush);
}

static void
stats_arena_pac_sec_print(emitter_t *emitter, unsigned i) {
	stats_arena_pac_sec_t sec;
	stats_gather_arena_pac_sec(i, &sec);
	stats_emit_arena_pac_sec(emitter, &sec);
}

static void
stats_emit_arena_hpa_counters(emitter_t *emitter,
    const stats_arena_hpa_counters_t *c, uint64_t uptime) {
	/* Merged pageslab / page counts (broken down by huginess below). */
	emitter_kv(emitter, "npageslabs", "npageslabs", emitter_type_size,
	    &c->npageslabs);
	emitter_kv(emitter, "nactive", "nactive", emitter_type_size,
	    &c->nactive);
	emitter_kv(emitter, "ndirty", "ndirty", emitter_type_size, &c->ndirty);

	uint64_t npurge_passes_ps = rate_per_second(c->npurge_passes, uptime);
	uint64_t npurges_ps = rate_per_second(c->npurges, uptime);
	uint64_t nhugifies_ps = rate_per_second(c->nhugifies, uptime);
	uint64_t nhugify_failures_ps =
	    rate_per_second(c->nhugify_failures, uptime);
	uint64_t ndehugifies_ps = rate_per_second(c->ndehugifies, uptime);
	emitter_kv_note(emitter, "npurge_passes", "npurge_passes",
	    emitter_type_uint64, &c->npurge_passes, "per_sec",
	    emitter_type_uint64, &npurge_passes_ps);
	emitter_kv_note(emitter, "npurges", "npurges", emitter_type_uint64,
	    &c->npurges, "per_sec", emitter_type_uint64, &npurges_ps);
	emitter_kv_note(emitter, "nhugifies", "nhugifies", emitter_type_uint64,
	    &c->nhugifies, "per_sec", emitter_type_uint64, &nhugifies_ps);
	emitter_kv_note(emitter, "nhugify_failures", "nhugify_failures",
	    emitter_type_uint64, &c->nhugify_failures, "per_sec",
	    emitter_type_uint64, &nhugify_failures_ps);
	emitter_kv_note(emitter, "ndehugifies", "ndehugifies",
	    emitter_type_uint64, &c->ndehugifies, "per_sec",
	    emitter_type_uint64, &ndehugifies_ps);

	emitter_dict_begin(emitter, "slabs", "slabs");
	emitter_kv(emitter, "npageslabs_nonhuge", "npageslabs_nonhuge",
	    emitter_type_size, &c->npageslabs_nonhuge);
	emitter_kv(emitter, "nactive_nonhuge", "nactive_nonhuge",
	    emitter_type_size, &c->nactive_nonhuge);
	emitter_kv(emitter, "ndirty_nonhuge", "ndirty_nonhuge",
	    emitter_type_size, &c->ndirty_nonhuge);
	emitter_kv(emitter, "nretained_nonhuge", "nretained_nonhuge",
	    emitter_type_size, &c->nretained_nonhuge);
	emitter_kv(emitter, "npageslabs_huge", "npageslabs_huge",
	    emitter_type_size, &c->npageslabs_huge);
	emitter_kv(emitter, "nactive_huge", "nactive_huge", emitter_type_size,
	    &c->nactive_huge);
	emitter_kv(emitter, "ndirty_huge", "ndirty_huge", emitter_type_size,
	    &c->ndirty_huge);
	emitter_dict_end(emitter);
}

static void
stats_arena_hpa_shard_counters_print(
    emitter_t *emitter, unsigned i, uint64_t uptime) {
	stats_arena_hpa_counters_t c;
	stats_gather_arena_hpa_counters(i, &c);
	stats_emit_arena_hpa_counters(emitter, &c, uptime);

	/*
	 * The extent-allocation distribution below is a small fixed-size
	 * (SEC_MAX_NALLOCS + 1) table with its own leaf ctls, so it is
	 * gathered into stack arrays and emitted (table then JSON) inline
	 * rather than through a stats_gather/stats_emit pair.
	 */
	uint64_t hpa_alloc_min_extents[SEC_MAX_NALLOCS + 1];
	uint64_t hpa_alloc_max_extents[SEC_MAX_NALLOCS + 1];
	uint64_t hpa_alloc_extents[SEC_MAX_NALLOCS + 1];
	uint64_t hpa_alloc_ps[SEC_MAX_NALLOCS + 1];
	uint64_t hpa_alloc_pages_per_ps[SEC_MAX_NALLOCS + 1];
	uint64_t hpa_alloc_extents_per_ps[SEC_MAX_NALLOCS + 1];
	uint64_t hpa_alloc_total_elapsed_ns_per_ps[SEC_MAX_NALLOCS + 1];

	size_t alloc_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(alloc_mib, 0, "stats.arenas");
	alloc_mib[2] = i;
	CTL_LEAF_PREPARE(alloc_mib, 3, "hpa_shard.alloc");

	for (size_t j = 0; j <= SEC_MAX_NALLOCS; j += 1) {
		alloc_mib[5] = j;
		CTL_LEAF(alloc_mib, 6, "min_extents", &hpa_alloc_min_extents[j],
		    uint64_t);
		CTL_LEAF(alloc_mib, 6, "max_extents", &hpa_alloc_max_extents[j],
		    uint64_t);
		CTL_LEAF(
		    alloc_mib, 6, "extents", &hpa_alloc_extents[j], uint64_t);
		CTL_LEAF(alloc_mib, 6, "ps", &hpa_alloc_ps[j], uint64_t);
		CTL_LEAF(alloc_mib, 6, "pages_per_ps",
		    &hpa_alloc_pages_per_ps[j], uint64_t);
		CTL_LEAF(alloc_mib, 6, "extents_per_ps",
		    &hpa_alloc_extents_per_ps[j], uint64_t);
		CTL_LEAF(alloc_mib, 6, "total_elapsed_ns_per_ps",
		    &hpa_alloc_total_elapsed_ns_per_ps[j], uint64_t);
	}

	emitter_table_printf(emitter, "  extent allocation distribution:\n");
	emitter_table_printf(emitter,
	    "  %4s %20s %20s %20s %20s %20s %20s %24s %24s\n", "",
	    "min_extents", "max_extents",
	    "extents", "ps", "pages_per_ps", "extents_per_ps",
	    "total_elapsed_ns_per_ps", "elapsed_ns_per_ps");
	for (size_t j = 0; j <= SEC_MAX_NALLOCS; j += 1) {
		const uint64_t extents_per_ps = hpa_alloc_extents_per_ps[j];
		const uint64_t total_elapsed_ns_per_ps =
		    hpa_alloc_total_elapsed_ns_per_ps[j];
		const uint64_t elapsed_ns_per_ps = (extents_per_ps != 0)
		    ? (total_elapsed_ns_per_ps / extents_per_ps)
		    : 0;
		emitter_table_printf(emitter,
		    "  %4zu %20" FMTu64 " %20" FMTu64 " %20" FMTu64
		    " %20" FMTu64 " %20" FMTu64 " %20" FMTu64 " %24" FMTu64
		    " %24" FMTu64 "\n",
		    j, hpa_alloc_min_extents[j], hpa_alloc_max_extents[j],
		    hpa_alloc_extents[j],
		    hpa_alloc_ps[j], hpa_alloc_pages_per_ps[j], extents_per_ps,
		    total_elapsed_ns_per_ps, elapsed_ns_per_ps);
	}
	emitter_table_printf(emitter, "\n");

	emitter_json_array_kv_begin(emitter, "extent_allocation_distribution");
	for (size_t j = 0; j <= SEC_MAX_NALLOCS; j += 1) {
		emitter_json_object_begin(emitter);
		emitter_json_kv(emitter, "min_extents", emitter_type_uint64,
		    &hpa_alloc_min_extents[j]);
		emitter_json_kv(emitter, "max_extents", emitter_type_uint64,
		    &hpa_alloc_max_extents[j]);
		emitter_json_kv(emitter, "extents", emitter_type_uint64,
		    &hpa_alloc_extents[j]);
		emitter_json_kv(
		    emitter, "ps", emitter_type_uint64, &hpa_alloc_ps[j]);
		emitter_json_kv(emitter, "pages_per_ps", emitter_type_uint64,
		    &hpa_alloc_pages_per_ps[j]);
		emitter_json_kv(emitter, "extents_per_ps", emitter_type_uint64,
		    &hpa_alloc_extents_per_ps[j]);
		emitter_json_kv(emitter, "total_elapsed_ns_per_ps",
		    emitter_type_uint64, &hpa_alloc_total_elapsed_ns_per_ps[j]);
		emitter_json_object_end(emitter);
	}
	emitter_json_array_end(emitter); /* End "alloc_batch" */
}

typedef struct {
	const stats_arena_hpa_slab_t *slab;
	const char                   *size_title;
	size_t                        size;
	size_t                        prev_size;
	unsigned                      ind;
} stats_arena_hpa_slab_emit_row_t;

#define HPA_SLAB_COL_GET(name)                                                \
	static void                                                            \
	stats_hpa_slab_col_get_##name(const void *vrow, emitter_col_t *col) {   \
		const stats_arena_hpa_slab_emit_row_t *row = vrow;               \
		col->size_val = row->slab->name;                                 \
	}
HPA_SLAB_COL_GET(npageslabs_huge)
HPA_SLAB_COL_GET(nactive_huge)
HPA_SLAB_COL_GET(ndirty_huge)
HPA_SLAB_COL_GET(npageslabs_nonhuge)
HPA_SLAB_COL_GET(nactive_nonhuge)
HPA_SLAB_COL_GET(ndirty_nonhuge)
HPA_SLAB_COL_GET(nretained_nonhuge)
#undef HPA_SLAB_COL_GET

static void
stats_hpa_slab_col_get_size(const void *vrow, emitter_col_t *col) {
	const stats_arena_hpa_slab_emit_row_t *row = vrow;
	if (row->size_title != NULL) {
		col->type = emitter_type_title;
		col->str_val = row->size_title;
	} else {
		col->size_val = row->size;
	}
}

static void
stats_hpa_slab_col_get_ind(const void *vrow, emitter_col_t *col) {
	const stats_arena_hpa_slab_emit_row_t *row = vrow;
	if (row->size_title != NULL) {
		col->type = emitter_type_title;
		col->str_val = "-";
	} else {
		col->unsigned_val = row->ind;
	}
}

enum {
	HPA_SLAB_COL_SIZE,
	HPA_SLAB_COL_IND,
	HPA_SLAB_COL_NPAGESLABS_HUGE,
	HPA_SLAB_COL_NACTIVE_HUGE,
	HPA_SLAB_COL_NDIRTY_HUGE,
	HPA_SLAB_COL_NPAGESLABS_NONHUGE,
	HPA_SLAB_COL_NACTIVE_NONHUGE,
	HPA_SLAB_COL_NDIRTY_NONHUGE,
	HPA_SLAB_COL_NRETAINED_NONHUGE,
	HPA_SLAB_COL_COUNT
};

#define HPA_SLAB_DESC(name, width)                                            \
	{#name, #name, emitter_justify_right, width, emitter_type_size,          \
	    STATS_COL_FLAG_NONE, stats_hpa_slab_col_get_##name}
static const emitter_col_desc_t stats_hpa_slab_cols[] = {
	{NULL, "size", emitter_justify_right, 20, emitter_type_size,
	    STATS_COL_FLAG_NONE, stats_hpa_slab_col_get_size},
	{NULL, "ind", emitter_justify_right, 4, emitter_type_unsigned,
	    STATS_COL_FLAG_NONE, stats_hpa_slab_col_get_ind},
	HPA_SLAB_DESC(npageslabs_huge, 16),
	HPA_SLAB_DESC(nactive_huge, 16),
	HPA_SLAB_DESC(ndirty_huge, 16),
	HPA_SLAB_DESC(npageslabs_nonhuge, 20),
	HPA_SLAB_DESC(nactive_nonhuge, 20),
	HPA_SLAB_DESC(ndirty_nonhuge, 20),
	HPA_SLAB_DESC(nretained_nonhuge, 20),
};
#undef HPA_SLAB_DESC
_Static_assert(
    sizeof(stats_hpa_slab_cols) / sizeof(stats_hpa_slab_cols[0]) ==
    HPA_SLAB_COL_COUNT,
    "stats_hpa_slab_cols must match HPA_SLAB_COL_COUNT");

static const unsigned stats_hpa_slab_json_order[] = {
	HPA_SLAB_COL_NPAGESLABS_HUGE,
	HPA_SLAB_COL_NACTIVE_HUGE,
	HPA_SLAB_COL_NDIRTY_HUGE,
	HPA_SLAB_COL_NPAGESLABS_NONHUGE,
	HPA_SLAB_COL_NACTIVE_NONHUGE,
	HPA_SLAB_COL_NDIRTY_NONHUGE,
	HPA_SLAB_COL_NRETAINED_NONHUGE,
};

static void
stats_emit_arena_hpa_slab_row(emitter_t *emitter,
    emitter_row_t *table_row, emitter_col_t *cols,
    const stats_arena_hpa_slab_emit_row_t *row, const char *json_key,
    char *size_buf, size_t size_buf_size, bool sparse, bool is_gap) {
	emitter_col_table_fill(stats_hpa_slab_cols, HPA_SLAB_COL_COUNT,
	    STATS_COL_FLAG_NONE, cols, row);
	if (row->size_title == NULL) {
		stats_size_col_set(&cols[HPA_SLAB_COL_SIZE], row->size,
		    row->prev_size, size_buf, size_buf_size);
	}
	if (json_key != NULL) {
		emitter_json_object_kv_begin(emitter, json_key);
	} else {
		emitter_json_object_begin(emitter);
	}
	emitter_col_table_emit_json(emitter, stats_hpa_slab_cols,
	    HPA_SLAB_COL_COUNT, STATS_COL_FLAG_NONE, cols,
	    stats_hpa_slab_json_order,
	    sizeof(stats_hpa_slab_json_order) /
	        sizeof(stats_hpa_slab_json_order[0]));
	emitter_json_object_end(emitter);
	if (sparse) {
		emitter_table_sparse_row(emitter, table_row, is_gap);
	} else {
		emitter_table_row(emitter, table_row);
	}
}

static void
stats_arena_hpa_shard_slabs_print(emitter_t *emitter, unsigned i) {
	emitter_row_t row, header_row;
	emitter_col_t cols[HPA_SLAB_COL_COUNT];
	emitter_col_t header_cols[HPA_SLAB_COL_COUNT];
	char size_buf[48];
	emitter_col_table_build(stats_hpa_slab_cols, HPA_SLAB_COL_COUNT,
	    STATS_COL_FLAG_NONE, &row, cols, &header_row, header_cols);

	emitter_table_printf(emitter, "pageslabs:\n");
	emitter_table_row(emitter, &header_row);

	/*
	 * Full and empty pageslabs are shown as two symbolic rows (size =
	 * "full"/"empty", no size-class index) at the top of the table; their
	 * JSON stays in the separate full_slabs / empty_slabs objects.
	 */
	stats_arena_hpa_slab_t sfull;
	stats_gather_arena_hpa_slab(i, "full_slabs", &sfull);
	stats_arena_hpa_slab_emit_row_t full_row = {
	    &sfull, "full", 0, 0, 0};
	stats_emit_arena_hpa_slab_row(emitter, &row, cols, &full_row,
	    "full_slabs", size_buf, sizeof(size_buf), false, false);

	stats_arena_hpa_slab_t sempty;
	stats_gather_arena_hpa_slab(i, "empty_slabs", &sempty);
	stats_arena_hpa_slab_emit_row_t empty_row = {
	    &sempty, "empty", 0, 0, 0};
	stats_emit_arena_hpa_slab_row(emitter, &row, cols, &empty_row,
	    "empty_slabs", size_buf, sizeof(size_buf), false, false);

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "hpa_shard.nonfull_slabs");

	emitter_json_array_kv_begin(emitter, "nonfull_slabs");
	emitter_table_sparse_begin(emitter);
	for (pszind_t j = 0; j < PSSET_NPSIZES && j < SC_NPSIZES; j++) {
		stats_arena_hpa_slab_t s;
		stats_gather_arena_hpa_nonfull(stats_arenas_mib, j, &s);

		bool is_gap = (s.npageslabs_huge == 0 && s.npageslabs_nonhuge == 0);

		stats_arena_hpa_slab_emit_row_t emit_row = {
		    &s, NULL, sz_pind2sz(j),
		    j > 0 ? sz_pind2sz(j - 1) : 0, j};
		stats_emit_arena_hpa_slab_row(emitter, &row, cols, &emit_row,
		    NULL, size_buf, sizeof(size_buf), true, is_gap);
	}
	emitter_json_array_end(emitter); /* End "nonfull_slabs" */
	emitter_table_sparse_end(emitter);
}

static void
stats_arena_hpa_shard_print(emitter_t *emitter, unsigned i, uint64_t uptime) {
	emitter_json_object_kv_begin(emitter, "hpa_shard");
	stats_arena_hpa_shard_sec_print(emitter, i);
	stats_arena_hpa_shard_counters_print(emitter, i, uptime);
	stats_arena_hpa_shard_slabs_print(emitter, i);
	emitter_json_object_end(emitter); /* End "hpa_shard" */
}

static void
stats_arena_mutexes_print(
    emitter_t *emitter, unsigned arena_ind, uint64_t uptime) {
	/*
	 * Mutex readers gather counters directly into emitter columns, coupling
	 * collection to the output representation.  Emission remains a separate
	 * mutex_stats_emit() call.
	 */
	emitter_row_t row;
	emitter_col_t col_name;
	emitter_col_t col64[mutex_prof_num_uint64_t_counters];
	emitter_col_t col32[mutex_prof_num_uint32_t_counters];

	emitter_row_init(&row);
	mutex_stats_init_cols(&row, "", &col_name, col64, col32);

	emitter_json_object_kv_begin(emitter, "mutexes");
	emitter_table_row(emitter, &row);

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = arena_ind;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "mutexes");

	for (mutex_prof_arena_ind_t i = 0; i < mutex_prof_num_arena_mutexes;
	    i++) {
		const char *name = arena_mutex_names[i];
		emitter_json_object_kv_begin(emitter, name);
		mutex_stats_read(
		    stats_arenas_mib, 4, name, &col_name, col64, col32, uptime);
		mutex_stats_emit(emitter, &row, col64, col32);
		emitter_json_object_end(emitter); /* Close the mutex dict. */
	}
	emitter_json_object_end(emitter); /* End "mutexes". */
}

static void
stats_emit_arena_basics(emitter_t *emitter, const stats_arena_basics_t *b) {
	if (b->has_name) {
		const char *namep = b->name;
		emitter_kv(
		    emitter, "name", "name", emitter_type_string, &namep);
	}
	emitter_kv(emitter, "nthreads", "assigned threads",
	    emitter_type_unsigned, &b->nthreads);
	emitter_kv(emitter, "uptime_ns", "uptime", emitter_type_uint64,
	    &b->uptime);
	emitter_kv(emitter, "dss", "dss allocation precedence",
	    emitter_type_string, &b->dss);
}

static uint64_t
stats_arena_basics_print(emitter_t *emitter, unsigned i) {
	stats_arena_basics_t basics;
	stats_gather_arena_basics(i, &basics);
	stats_emit_arena_basics(emitter, &basics);
	return basics.uptime;
}

static void
stats_emit_arena_decay(emitter_t *emitter, const stats_arena_decay_t *d) {
	emitter_row_t decay_row;
	emitter_row_init(&decay_row);

	/* JSON-style emission. */
	emitter_json_kv(
	    emitter, "dirty_decay_ms", emitter_type_ssize, &d->dirty_decay_ms);
	emitter_json_kv(
	    emitter, "muzzy_decay_ms", emitter_type_ssize, &d->muzzy_decay_ms);

	emitter_json_kv(emitter, "pactive", emitter_type_size, &d->pactive);
	emitter_json_kv(emitter, "pdirty", emitter_type_size, &d->pdirty);
	emitter_json_kv(emitter, "pmuzzy", emitter_type_size, &d->pmuzzy);

	emitter_json_kv(
	    emitter, "dirty_npurge", emitter_type_uint64, &d->dirty_npurge);
	emitter_json_kv(
	    emitter, "dirty_nmadvise", emitter_type_uint64, &d->dirty_nmadvise);
	emitter_json_kv(
	    emitter, "dirty_purged", emitter_type_uint64, &d->dirty_purged);

	emitter_json_kv(
	    emitter, "muzzy_npurge", emitter_type_uint64, &d->muzzy_npurge);
	emitter_json_kv(
	    emitter, "muzzy_nmadvise", emitter_type_uint64, &d->muzzy_nmadvise);
	emitter_json_kv(
	    emitter, "muzzy_purged", emitter_type_uint64, &d->muzzy_purged);

	/* Table-style emission. */
	COL(decay_row, decay_type, right, 9, title);
	col_decay_type.str_val = "decaying:";

	COL(decay_row, decay_time, right, 6, title);
	col_decay_time.str_val = "time";

	COL(decay_row, decay_npages, right, 13, title);
	col_decay_npages.str_val = "npages";

	COL(decay_row, decay_sweeps, right, 13, title);
	col_decay_sweeps.str_val = "sweeps";

	COL(decay_row, decay_madvises, right, 13, title);
	col_decay_madvises.str_val = "madvises";

	COL(decay_row, decay_purged, right, 13, title);
	col_decay_purged.str_val = "purged";

	/* Title row. */
	emitter_table_row(emitter, &decay_row);

	/* Dirty row. */
	col_decay_type.str_val = "dirty:";

	if (d->dirty_decay_ms >= 0) {
		col_decay_time.type = emitter_type_ssize;
		col_decay_time.ssize_val = d->dirty_decay_ms;
	} else {
		col_decay_time.type = emitter_type_title;
		col_decay_time.str_val = "N/A";
	}

	col_decay_npages.type = emitter_type_size;
	col_decay_npages.size_val = d->pdirty;

	col_decay_sweeps.type = emitter_type_uint64;
	col_decay_sweeps.uint64_val = d->dirty_npurge;

	col_decay_madvises.type = emitter_type_uint64;
	col_decay_madvises.uint64_val = d->dirty_nmadvise;

	col_decay_purged.type = emitter_type_uint64;
	col_decay_purged.uint64_val = d->dirty_purged;

	emitter_table_row(emitter, &decay_row);

	/* Muzzy row. */
	col_decay_type.str_val = "muzzy:";

	if (d->muzzy_decay_ms >= 0) {
		col_decay_time.type = emitter_type_ssize;
		col_decay_time.ssize_val = d->muzzy_decay_ms;
	} else {
		col_decay_time.type = emitter_type_title;
		col_decay_time.str_val = "N/A";
	}

	col_decay_npages.type = emitter_type_size;
	col_decay_npages.size_val = d->pmuzzy;

	col_decay_sweeps.type = emitter_type_uint64;
	col_decay_sweeps.uint64_val = d->muzzy_npurge;

	col_decay_madvises.type = emitter_type_uint64;
	col_decay_madvises.uint64_val = d->muzzy_nmadvise;

	col_decay_purged.type = emitter_type_uint64;
	col_decay_purged.uint64_val = d->muzzy_purged;

	emitter_table_row(emitter, &decay_row);
}

static size_t
stats_arena_decay_print(emitter_t *emitter, unsigned i) {
	stats_arena_decay_t decay;
	stats_gather_arena_decay(i, &decay);
	stats_emit_arena_decay(emitter, &decay);
	return decay.pactive;
}

static void
stats_emit_arena_alloc(emitter_t *emitter, const stats_arena_alloc_t *small,
    const stats_arena_alloc_t *large, uint64_t uptime) {
	emitter_row_t alloc_count_row;
	emitter_row_init(&alloc_count_row);

	COL(alloc_count_row, count_title, left, 21, title);
	col_count_title.str_val = "";

	COL(alloc_count_row, count_allocated, right, 16, title);
	col_count_allocated.str_val = "allocated";

	COL(alloc_count_row, count_nmalloc, right, 16, title);
	col_count_nmalloc.str_val = "nmalloc";
	COL(alloc_count_row, count_nmalloc_ps, right, 10, title);
	col_count_nmalloc_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_ndalloc, right, 16, title);
	col_count_ndalloc.str_val = "ndalloc";
	COL(alloc_count_row, count_ndalloc_ps, right, 10, title);
	col_count_ndalloc_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_nrequests, right, 16, title);
	col_count_nrequests.str_val = "nrequests";
	COL(alloc_count_row, count_nrequests_ps, right, 10, title);
	col_count_nrequests_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_nfills, right, 16, title);
	col_count_nfills.str_val = "nfill";
	COL(alloc_count_row, count_nfills_ps, right, 10, title);
	col_count_nfills_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_nflushes, right, 16, title);
	col_count_nflushes.str_val = "nflush";
	COL(alloc_count_row, count_nflushes_ps, right, 10, title);
	col_count_nflushes_ps.str_val = "(#/sec)";

	emitter_table_row(emitter, &alloc_count_row);

	col_count_nmalloc_ps.type = emitter_type_uint64;
	col_count_ndalloc_ps.type = emitter_type_uint64;
	col_count_nrequests_ps.type = emitter_type_uint64;
	col_count_nfills_ps.type = emitter_type_uint64;
	col_count_nflushes_ps.type = emitter_type_uint64;

	/*
	 * JSON keys for the value columns; emitter_row() emits these as kvs.
	 * The (#/sec) rate columns and the title column stay table-only (no
	 * json_key).
	 */
	col_count_allocated.json_key = "allocated";
	col_count_nmalloc.json_key = "nmalloc";
	col_count_ndalloc.json_key = "ndalloc";
	col_count_nrequests.json_key = "nrequests";
	col_count_nfills.json_key = "nfills";
	col_count_nflushes.json_key = "nflushes";

#define EMIT_ALLOC_STAT(a, name, valtype)                                      \
	col_count_##name.type = emitter_type_##valtype;                        \
	col_count_##name.valtype##_val = (a)->name;

	emitter_json_object_kv_begin(emitter, "small");
	col_count_title.str_val = "small:";

	EMIT_ALLOC_STAT(small, allocated, size)
	EMIT_ALLOC_STAT(small, nmalloc, uint64)
	col_count_nmalloc_ps.uint64_val = rate_per_second(
	    col_count_nmalloc.uint64_val, uptime);
	EMIT_ALLOC_STAT(small, ndalloc, uint64)
	col_count_ndalloc_ps.uint64_val = rate_per_second(
	    col_count_ndalloc.uint64_val, uptime);
	EMIT_ALLOC_STAT(small, nrequests, uint64)
	col_count_nrequests_ps.uint64_val = rate_per_second(
	    col_count_nrequests.uint64_val, uptime);
	EMIT_ALLOC_STAT(small, nfills, uint64)
	col_count_nfills_ps.uint64_val = rate_per_second(
	    col_count_nfills.uint64_val, uptime);
	EMIT_ALLOC_STAT(small, nflushes, uint64)
	col_count_nflushes_ps.uint64_val = rate_per_second(
	    col_count_nflushes.uint64_val, uptime);

	emitter_row(emitter, &alloc_count_row);
	emitter_json_object_end(emitter); /* Close "small". */

	emitter_json_object_kv_begin(emitter, "large");
	col_count_title.str_val = "large:";

	EMIT_ALLOC_STAT(large, allocated, size)
	EMIT_ALLOC_STAT(large, nmalloc, uint64)
	col_count_nmalloc_ps.uint64_val = rate_per_second(
	    col_count_nmalloc.uint64_val, uptime);
	EMIT_ALLOC_STAT(large, ndalloc, uint64)
	col_count_ndalloc_ps.uint64_val = rate_per_second(
	    col_count_ndalloc.uint64_val, uptime);
	EMIT_ALLOC_STAT(large, nrequests, uint64)
	col_count_nrequests_ps.uint64_val = rate_per_second(
	    col_count_nrequests.uint64_val, uptime);
	EMIT_ALLOC_STAT(large, nfills, uint64)
	col_count_nfills_ps.uint64_val = rate_per_second(
	    col_count_nfills.uint64_val, uptime);
	EMIT_ALLOC_STAT(large, nflushes, uint64)
	col_count_nflushes_ps.uint64_val = rate_per_second(
	    col_count_nflushes.uint64_val, uptime);

	emitter_row(emitter, &alloc_count_row);
	emitter_json_object_end(emitter); /* Close "large". */

#undef EMIT_ALLOC_STAT

	/* Aggregated small + large stats are emitter only in table mode. */
	col_count_title.str_val = "total:";
	col_count_allocated.size_val = small->allocated + large->allocated;
	col_count_nmalloc.uint64_val = small->nmalloc + large->nmalloc;
	col_count_ndalloc.uint64_val = small->ndalloc + large->ndalloc;
	col_count_nrequests.uint64_val = small->nrequests + large->nrequests;
	col_count_nfills.uint64_val = small->nfills + large->nfills;
	col_count_nflushes.uint64_val = small->nflushes + large->nflushes;
	col_count_nmalloc_ps.uint64_val = rate_per_second(
	    col_count_nmalloc.uint64_val, uptime);
	col_count_ndalloc_ps.uint64_val = rate_per_second(
	    col_count_ndalloc.uint64_val, uptime);
	col_count_nrequests_ps.uint64_val = rate_per_second(
	    col_count_nrequests.uint64_val, uptime);
	col_count_nfills_ps.uint64_val = rate_per_second(
	    col_count_nfills.uint64_val, uptime);
	col_count_nflushes_ps.uint64_val = rate_per_second(
	    col_count_nflushes.uint64_val, uptime);
	emitter_table_row(emitter, &alloc_count_row);
}

static void
stats_arena_alloc_print(emitter_t *emitter, unsigned i, uint64_t uptime) {
	stats_arena_alloc_t small_alloc, large_alloc;
	stats_gather_arena_alloc(i, &small_alloc, &large_alloc);
	stats_emit_arena_alloc(emitter, &small_alloc, &large_alloc, uptime);
}

static void
stats_emit_arena_mem(emitter_t *emitter, const stats_arena_mem_t *mem,
    size_t active_bytes) {
	emitter_row_t mem_count_row;
	emitter_row_init(&mem_count_row);

	emitter_col_t mem_count_title;
	emitter_col_init(&mem_count_title, &mem_count_row);
	mem_count_title.justify = emitter_justify_left;
	mem_count_title.width = 21;
	mem_count_title.type = emitter_type_title;
	mem_count_title.str_val = "";

	emitter_col_t mem_count_val;
	emitter_col_init(&mem_count_val, &mem_count_row);
	mem_count_val.justify = emitter_justify_right;
	mem_count_val.width = 16;
	mem_count_val.type = emitter_type_title;
	mem_count_val.str_val = "";

	/* Blank spacer row (both columns are empty titles; table only). */
	emitter_row(emitter, &mem_count_row);
	mem_count_val.type = emitter_type_size;

	/*
	 * Active count in bytes is table only: mem_count_val carries no
	 * json_key, so emitter_row() skips it in JSON.
	 */
	mem_count_title.str_val = "active:";
	mem_count_val.size_val = active_bytes;
	emitter_row(emitter, &mem_count_row);

	/*
	 * Each remaining stat is one row: the value column's json_key makes it a
	 * "<stat>": value pair in JSON and the aligned value in the table.
	 */
#define EMIT_MEM_STAT(stat)                                                    \
	mem_count_title.str_val = #stat ":";                                   \
	mem_count_val.json_key = #stat;                                        \
	mem_count_val.size_val = mem->stat;                                    \
	emitter_row(emitter, &mem_count_row);

	EMIT_MEM_STAT(mapped)
	EMIT_MEM_STAT(retained)
	EMIT_MEM_STAT(pinned)
	EMIT_MEM_STAT(base)
	EMIT_MEM_STAT(internal)
	EMIT_MEM_STAT(metadata_edata)
	EMIT_MEM_STAT(metadata_rtree)
	EMIT_MEM_STAT(metadata_thp)
	EMIT_MEM_STAT(tcache_bytes)
	EMIT_MEM_STAT(tcache_stashed_bytes)
	EMIT_MEM_STAT(resident)
	EMIT_MEM_STAT(abandoned_vm)
	EMIT_MEM_STAT(extent_avail)
#undef EMIT_MEM_STAT
}

static void
stats_arena_mem_print(emitter_t *emitter, unsigned i, size_t pactive) {
	stats_arena_mem_t m;
	stats_gather_arena_mem(i, &m);
	stats_emit_arena_mem(emitter, &m, pactive * m.page);
}

JEMALLOC_COLD
static void
stats_arena_print(emitter_t *emitter, unsigned i, bool bins, bool large,
    bool mutex, bool extents, bool hpa) {
	uint64_t uptime = stats_arena_basics_print(emitter, i);
	size_t   pactive = stats_arena_decay_print(emitter, i);
	stats_arena_alloc_print(emitter, i, uptime);
	stats_arena_mem_print(emitter, i, pactive);

	if (opt_pac_sec_opts.nshards > 0) {
		stats_arena_pac_sec_print(emitter, i);
	}

	if (mutex) {
		stats_arena_mutexes_print(emitter, i, uptime);
	}
	if (bins) {
		stats_arena_bins_print(emitter, mutex, i, uptime);
	}
	if (large) {
		stats_arena_lextents_print(emitter, i, uptime);
	}
	if (extents) {
		stats_arena_extents_print(emitter, i);
	}
	if (hpa) {
		stats_arena_hpa_shard_print(emitter, i, uptime);
	}
}

static void
stats_general_version(emitter_t *emitter) {
	const char *cpv;

	CTL_GET("version", &cpv, const char *);
	emitter_kv(emitter, "version", "Version", emitter_type_string, &cpv);
}

static void
stats_general_config(emitter_t *emitter) {
	/*
	 * Most config entries are independent scalars that
	 * CONFIG_WRITE_BOOL reads and emits together.  Keeping those operations
	 * local is clearer than introducing a gather struct for this flat list.
	 */
	bool bv;

	emitter_dict_begin(emitter, "config", "Build-time option settings");
	CONFIG_WRITE_BOOL(cache_oblivious);
	CONFIG_WRITE_BOOL(debug);
	CONFIG_WRITE_BOOL(fill);
	CONFIG_WRITE_BOOL(infallible_new);
	CONFIG_WRITE_BOOL(lazy_lock);
	emitter_kv(emitter, "malloc_conf", "config.malloc_conf",
	    emitter_type_string, &config_malloc_conf);

	CONFIG_WRITE_BOOL(opt_safety_checks);
	CONFIG_WRITE_BOOL(prof);
	CONFIG_WRITE_BOOL(prof_libgcc);
	CONFIG_WRITE_BOOL(prof_libunwind);
	CONFIG_WRITE_BOOL(stats);
	CONFIG_WRITE_BOOL(utrace);
	CONFIG_WRITE_BOOL(xmalloc);
	emitter_dict_end(emitter); /* Close "config" dict. */
}

static void
stats_general_system(emitter_t *emitter) {
	emitter_dict_begin(emitter, "system", "System configuration");

	/*
	 * This shows system's THP mode detected at jemalloc's init time.
	 * jemalloc does not re-detect the mode even if it changes after
	 * jemalloc's init.  It is assumed that system's THP mode is stable
	 * during the process's lifetime and a violation could lead to
	 * undefined behavior.
	*/
	const char *thp_mode_name = system_thp_mode_names[init_system_thp_mode];
	emitter_kv(emitter, "thp_mode", "system.thp_mode", emitter_type_string,
	    &thp_mode_name);

	emitter_dict_end(emitter); /* Close "system". */
}

static void
stats_general_opts(emitter_t *emitter) {
	/*
	 * The OPT_WRITE_* macros conditionally read and emit most options.
	 * Options needing special formatting or availability checks stay
	 * explicit below.
	 */
	const char *cpv;
	bool        bv, bv2;
	unsigned    uv;
	uint32_t    u32v;
	uint64_t    u64v;
	int64_t     i64v;
	ssize_t     ssv, ssv2;
	size_t      sv, bsz, usz, u32sz, u64sz, i64sz, ssz, sssz, cpsz;

	bsz = sizeof(bool);
	usz = sizeof(unsigned);
	ssz = sizeof(size_t);
	sssz = sizeof(ssize_t);
	cpsz = sizeof(const char *);
	u32sz = sizeof(uint32_t);
	i64sz = sizeof(int64_t);
	u64sz = sizeof(uint64_t);
	emitter_dict_begin(emitter, "opt", "Run-time option settings");

	/*
	 * opt.malloc_conf.
	 *
	 * Sources are documented in https://jemalloc.net/jemalloc.3.html#tuning
	 * - (Not Included Here) The string specified via --with-malloc-conf,
	 *     which is already printed out above as config.malloc_conf
	 * - (Included) The string pointed to by the global variable malloc_conf
	 * - (Included) The “name” of the file referenced by the symbolic link
	 *     named /etc/malloc.conf
	 * - (Included) The value of the environment variable MALLOC_CONF
	 * - (Optional, Unofficial) The string pointed to by the global variable
	 *     malloc_conf_2_conf_harder, which is hidden from the public.
	 *
	 * Note: The outputs are strictly ordered by priorities (low -> high).
	 *
	 */
#define MALLOC_CONF_WRITE(name, message)                                       \
	if (je_mallctl("opt.malloc_conf." name, (void *)&cpv, &cpsz, NULL, 0)  \
	    != 0) {                                                            \
		cpv = "";                                                      \
	}                                                                      \
	emitter_kv(emitter, name, message, emitter_type_string, &cpv);

	MALLOC_CONF_WRITE("global_var", "Global variable malloc_conf");
	MALLOC_CONF_WRITE("symlink", "Symbolic link malloc.conf");
	MALLOC_CONF_WRITE("env_var", "Environment variable MALLOC_CONF");
	/* As this config is unofficial, skip the output if it's NULL. */
	if (je_malloc_conf_2_conf_harder != NULL) {
		cpv = je_malloc_conf_2_conf_harder;
		emitter_kv(emitter, "global_var_2_conf_harder",
		    "Global "
		    "variable malloc_conf_2_conf_harder",
		    emitter_type_string, &cpv);
	}
#undef MALLOC_CONF_WRITE

	OPT_WRITE_BOOL("abort")
	OPT_WRITE_BOOL("abort_conf")
	OPT_WRITE_BOOL("cache_oblivious")
	OPT_WRITE_BOOL("confirm_conf")
	OPT_WRITE_BOOL("experimental_hpa_start_huge_if_thp_always")
	OPT_WRITE_BOOL("experimental_hpa_enforce_hugify")
	OPT_WRITE_BOOL("retain")
	OPT_WRITE_CHAR_P("dss")
	OPT_WRITE_UNSIGNED("narenas")
	OPT_WRITE_CHAR_P("percpu_arena")
	OPT_WRITE_SIZE_T("oversize_threshold")
	OPT_WRITE_BOOL("hpa")
	OPT_WRITE_SIZE_T("hpa_slab_max_alloc")
	OPT_WRITE_SIZE_T("hpa_hugification_threshold")
	OPT_WRITE_UINT64("hpa_hugify_delay_ms")
	OPT_WRITE_BOOL("hpa_hugify_sync")
	OPT_WRITE_UINT64("hpa_min_purge_interval_ms")
	if (je_mallctl("opt.hpa_dirty_mult", (void *)&u32v, &u32sz, NULL, 0)
	    == 0) {
		/*
		 * We cheat a little and "know" the secret meaning of this
		 * representation.
		 */
		if (u32v == (uint32_t)-1) {
			const char *neg1 = "-1";
			emitter_kv(emitter, "hpa_dirty_mult",
			    "opt.hpa_dirty_mult", emitter_type_string, &neg1);
		} else {
			char buf[FXP_BUF_SIZE];
			fxp_print(u32v, buf);
			const char *bufp = buf;
			emitter_kv(emitter, "hpa_dirty_mult",
			    "opt.hpa_dirty_mult", emitter_type_string, &bufp);
		}
	}
	OPT_WRITE_SIZE_T("hpa_purge_threshold")
	OPT_WRITE_UINT64("hpa_min_purge_delay_ms")
	OPT_WRITE_CHAR_P("hpa_hugify_style")
	OPT_WRITE_SIZE_T("hpa_sec_nshards")
	OPT_WRITE_SIZE_T("hpa_sec_max_alloc")
	OPT_WRITE_SIZE_T("hpa_sec_max_bytes")
	OPT_WRITE_SIZE_T("experimental_pac_sec_nshards")
	OPT_WRITE_SIZE_T("experimental_pac_sec_max_alloc")
	OPT_WRITE_SIZE_T("experimental_pac_sec_max_bytes")
	OPT_WRITE_BOOL("huge_arena_pac_thp")
	OPT_WRITE_CHAR_P("metadata_thp")
	OPT_WRITE_INT64("mutex_max_spin")
	OPT_WRITE_BOOL_MUTABLE("background_thread", "background_thread")
	OPT_WRITE_SSIZE_T_MUTABLE("dirty_decay_ms", "arenas.dirty_decay_ms")
	OPT_WRITE_SSIZE_T_MUTABLE("muzzy_decay_ms", "arenas.muzzy_decay_ms")
	OPT_WRITE_SIZE_T("lg_extent_max_active_fit")
	OPT_WRITE_CHAR_P("junk")
	OPT_WRITE_BOOL("zero")
	OPT_WRITE_BOOL("utrace")
	OPT_WRITE_BOOL("xmalloc")
	OPT_WRITE_BOOL("experimental_tcache_gc")
	OPT_WRITE_BOOL("tcache")
	OPT_WRITE_SIZE_T("tcache_max")
	OPT_WRITE_UNSIGNED("tcache_nslots_small_min")
	OPT_WRITE_UNSIGNED("tcache_nslots_small_max")
	OPT_WRITE_UNSIGNED("tcache_nslots_large")
	OPT_WRITE_SSIZE_T("lg_tcache_nslots_mul")
	OPT_WRITE_SIZE_T("tcache_gc_incr_bytes")
	OPT_WRITE_SIZE_T("tcache_gc_delay_bytes")
	OPT_WRITE_UNSIGNED("lg_tcache_flush_small_div")
	OPT_WRITE_UNSIGNED("lg_tcache_flush_large_div")
	OPT_WRITE_UNSIGNED("debug_double_free_max_scan")
	OPT_WRITE_CHAR_P("thp")
	OPT_WRITE_BOOL("prof")
	OPT_WRITE_UNSIGNED("prof_bt_max")
	OPT_WRITE_CHAR_P("prof_prefix")
	OPT_WRITE_BOOL_MUTABLE("prof_active", "prof.active")
	OPT_WRITE_BOOL_MUTABLE(
	    "prof_thread_active_init", "prof.thread_active_init")
	OPT_WRITE_SSIZE_T_MUTABLE("lg_prof_sample", "prof.lg_sample")
	OPT_WRITE_BOOL("prof_accum")
	OPT_WRITE_SSIZE_T("lg_prof_interval")
	OPT_WRITE_BOOL("prof_gdump")
	OPT_WRITE_BOOL("prof_final")
	OPT_WRITE_BOOL("prof_leak")
	OPT_WRITE_BOOL("prof_leak_error")
	OPT_WRITE_BOOL("stats_print")
	OPT_WRITE_CHAR_P("stats_print_opts")
	OPT_WRITE_INT64("stats_interval")
	OPT_WRITE_CHAR_P("stats_interval_opts")
	OPT_WRITE_CHAR_P("zero_realloc")
	OPT_WRITE_SIZE_T("process_madvise_max_batch")
	OPT_WRITE_BOOL("disable_large_size_classes")

	emitter_dict_end(emitter); /* Close "opt". */
}

static void
stats_general_prof(emitter_t *emitter) {
	if (config_prof) {
		bool bv;
		uint64_t u64v;
		ssize_t ssv;

		emitter_dict_begin(emitter, "prof", "Profiling settings");

		CTL_GET("prof.thread_active_init", &bv, bool);
		emitter_kv(emitter, "thread_active_init",
		    "prof.thread_active_init", emitter_type_bool, &bv);

		CTL_GET("prof.active", &bv, bool);
		emitter_kv(
		    emitter, "active", "prof.active", emitter_type_bool, &bv);

		CTL_GET("prof.gdump", &bv, bool);
		emitter_kv(
		    emitter, "gdump", "prof.gdump", emitter_type_bool, &bv);

		CTL_GET("prof.interval", &u64v, uint64_t);
		emitter_kv(emitter, "interval", "prof.interval",
		    emitter_type_uint64, &u64v);

		CTL_GET("prof.lg_sample", &ssv, ssize_t);
		emitter_kv(emitter, "lg_sample", "prof.lg_sample",
		    emitter_type_ssize, &ssv);

		emitter_dict_end(emitter); /* Close "prof". */
	}
}

static void
stats_general_bin_meta_print(emitter_t *emitter, unsigned nbins) {
	/*
	 * Streaming JSON-only size-class metadata table (per-row gather+emit);
	 * see stats_arena_bins_print for the streaming rationale.
	 */
	emitter_json_array_kv_begin(emitter, "bin");
	size_t mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(mib, 0, "arenas.bin");
	for (unsigned i = 0; i < nbins; i++) {
		stats_arena_bin_meta_t bm;
		stats_gather_arena_bin_meta(mib, i, &bm);
		emitter_json_object_begin(emitter);
		emitter_json_kv(emitter, "size", emitter_type_size, &bm.size);
		emitter_json_kv(emitter, "nregs", emitter_type_uint32, &bm.nregs);
		emitter_json_kv(
		    emitter, "slab_size", emitter_type_size, &bm.slab_size);
		emitter_json_kv(emitter, "nshards", emitter_type_uint32, &bm.nshards);
		emitter_json_object_end(emitter);
	}
	emitter_json_array_end(emitter); /* Close "bin". */
}

static void
stats_general_lextent_meta_print(emitter_t *emitter, unsigned nlextents) {
	/* Streaming JSON-only metadata table; see stats_general_bin_meta_print. */
	emitter_json_array_kv_begin(emitter, "lextent");
	size_t mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(mib, 0, "arenas.lextent");
	for (unsigned i = 0; i < nlextents; i++) {
		stats_arena_lextent_meta_t lm;
		stats_gather_arena_lextent_meta(mib, i, &lm);
		emitter_json_object_begin(emitter);
		emitter_json_kv(emitter, "size", emitter_type_size, &lm.size);
		emitter_json_object_end(emitter);
	}
	emitter_json_array_end(emitter); /* Close "lextent". */
}

static void
stats_general_arenas_print(emitter_t *emitter, bool omit_size_class_meta) {
	stats_arena_config_t cfg;
	stats_gather_arena_config(&cfg);

	/*
	 * The json output sticks arena info into an "arenas" dict; the table
	 * output puts them at the top-level.
	 */
	emitter_json_object_kv_begin(emitter, "arenas");

	emitter_kv(
	    emitter, "narenas", "Arenas", emitter_type_unsigned, &cfg.narenas);

	/*
	 * Decay settings are emitted only in json mode; in table mode, they
	 * are emitted as notes with the opt output, above.
	 */
	emitter_json_kv(
	    emitter, "dirty_decay_ms", emitter_type_ssize, &cfg.dirty_decay_ms);
	emitter_json_kv(
	    emitter, "muzzy_decay_ms", emitter_type_ssize, &cfg.muzzy_decay_ms);

	emitter_kv(emitter, "quantum", "Quantum size", emitter_type_size,
	    &cfg.quantum);
	emitter_kv(emitter, "page", "Page size", emitter_type_size, &cfg.page);
	emitter_kv(emitter, "hugepage", "Hugepage size", emitter_type_size,
	    &cfg.hugepage);

	if (cfg.have_tcache_max) {
		emitter_kv(emitter, "tcache_max",
		    "Maximum thread-cached size class", emitter_type_size,
		    &cfg.tcache_max);
	}

	emitter_kv(emitter, "nbins", "Number of bin size classes",
	    emitter_type_unsigned, &cfg.nbins);
	emitter_kv(emitter, "nhbins", "Number of thread-cache bin size classes",
	    emitter_type_unsigned, &cfg.nhbins);

	/*
	 * Per-size-class geometry is omitted from table output.  It is present
	 * in JSON whenever general output is enabled; when omitted, it is not
	 * gathered either.
	 */
	if (!omit_size_class_meta) {
		stats_general_bin_meta_print(emitter, cfg.nbins);
	}

	emitter_kv(emitter, "nlextents", "Number of large size classes",
	    emitter_type_unsigned, &cfg.nlextents);

	if (!omit_size_class_meta) {
		stats_general_lextent_meta_print(emitter, cfg.nlextents);
	}

	emitter_json_object_end(emitter); /* Close "arenas" */
}

JEMALLOC_COLD
static void
stats_general_print(emitter_t *emitter, bool omit_size_class_meta) {
	stats_general_version(emitter);
	stats_general_config(emitter);
	stats_general_system(emitter);
	stats_general_opts(emitter);
	stats_general_prof(emitter);
	stats_general_arenas_print(emitter, omit_size_class_meta);
}

static void
stats_emit_global(emitter_t *emitter, const stats_global_t *g) {
	/* Generic global stats. */
	emitter_json_kv(emitter, "allocated", emitter_type_size, &g->allocated);
	emitter_json_kv(emitter, "active", emitter_type_size, &g->active);
	emitter_json_kv(emitter, "metadata", emitter_type_size, &g->metadata);
	emitter_json_kv(
	    emitter, "metadata_edata", emitter_type_size, &g->metadata_edata);
	emitter_json_kv(
	    emitter, "metadata_rtree", emitter_type_size, &g->metadata_rtree);
	emitter_json_kv(
	    emitter, "metadata_thp", emitter_type_size, &g->metadata_thp);
	emitter_json_kv(emitter, "resident", emitter_type_size, &g->resident);
	emitter_json_kv(emitter, "mapped", emitter_type_size, &g->mapped);
	emitter_json_kv(emitter, "retained", emitter_type_size, &g->retained);
	emitter_json_kv(emitter, "pinned", emitter_type_size, &g->pinned);
	emitter_json_kv(
	    emitter, "zero_reallocs", emitter_type_size, &g->zero_reallocs);

	emitter_table_printf(emitter,
	    "Allocated: %zu, active: %zu, "
	    "metadata: %zu (n_thp %zu, edata %zu, rtree %zu), resident: %zu, "
	    "mapped: %zu, retained: %zu, pinned: %zu\n",
	    g->allocated, g->active, g->metadata, g->metadata_thp,
	    g->metadata_edata, g->metadata_rtree, g->resident, g->mapped,
	    g->retained, g->pinned);

	/* Strange behaviors */
	emitter_table_printf(emitter,
	    "Count of realloc(non-null-ptr, 0) calls: %zu\n", g->zero_reallocs);

	/* Background thread stats. */
	emitter_json_object_kv_begin(emitter, "background_thread");
	emitter_json_kv(emitter, "num_threads", emitter_type_size,
	    &g->num_background_threads);
	emitter_json_kv(emitter, "num_runs", emitter_type_uint64,
	    &g->background_thread_num_runs);
	emitter_json_kv(emitter, "run_interval", emitter_type_uint64,
	    &g->background_thread_run_interval);
	emitter_json_object_end(emitter); /* Close "background_thread". */

	emitter_table_printf(emitter,
	    "Background threads: %zu, "
	    "num_runs: %" FMTu64 ", run_interval: %" FMTu64 " ns\n",
	    g->num_background_threads, g->background_thread_num_runs,
	    g->background_thread_run_interval);
}

static void
stats_global_mutexes_print(emitter_t *emitter) {
	/* Counters are gathered into emitter columns; see the arena variant. */
	emitter_row_t row;
	emitter_col_t name;
	emitter_col_t col64[mutex_prof_num_uint64_t_counters];
	emitter_col_t col32[mutex_prof_num_uint32_t_counters];
	uint64_t      uptime;

	emitter_row_init(&row);
	mutex_stats_init_cols(&row, "", &name, col64, col32);

	emitter_table_row(emitter, &row);
	emitter_json_object_kv_begin(emitter, "mutexes");

	CTL_M2_GET("stats.arenas.0.uptime", 0, &uptime, uint64_t);

	size_t stats_mutexes_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_mutexes_mib, 0, "stats.mutexes");
	for (int i = 0; i < mutex_prof_num_global_mutexes; i++) {
		mutex_stats_read(stats_mutexes_mib, 2,
		    global_mutex_names[i], &name, col64, col32, uptime);
		emitter_json_object_kv_begin(emitter, global_mutex_names[i]);
		mutex_stats_emit(emitter, &row, col64, col32);
		emitter_json_object_end(emitter);
	}

	emitter_json_object_end(emitter); /* Close "mutexes". */
}

static void
stats_print_globals(emitter_t *emitter, bool mutex) {
	stats_global_t g;
	stats_gather_global(&g);

	emitter_json_object_kv_begin(emitter, "stats");
	stats_emit_global(emitter, &g);
	if (mutex) {
		stats_global_mutexes_print(emitter);
	}
	emitter_json_object_end(emitter); /* Close "stats". */
}

static void
stats_print_one_arena(emitter_t *emitter, unsigned arena_ind,
    const char *json_key, const char *table_header, bool bins, bool large,
    bool mutex, bool extents, bool hpa) {
	emitter_json_object_kv_begin(emitter, json_key);
	emitter_table_printf(emitter, "%s", table_header);
	stats_arena_print(emitter, arena_ind, bins, large, mutex, extents, hpa);
	emitter_json_object_end(emitter);
}

JEMALLOC_COLD
static void
stats_print_runtime_stats(emitter_t *emitter, bool merged, bool destroyed,
    bool unmerged, bool bins, bool large, bool mutex, bool extents, bool hpa) {
	stats_print_globals(emitter, mutex);

	if (!merged && !destroyed && !unmerged) {
		return;
	}

	unsigned narenas;
	CTL_GET("arenas.narenas", &narenas, unsigned);
	VARIABLE_ARRAY_UNSAFE(bool, initialized, narenas);
	bool destroyed_initialized;
	unsigned ninitialized = stats_gather_arenas_initialized(narenas,
	    initialized, &destroyed_initialized);

	emitter_json_object_kv_begin(emitter, "stats.arenas");

	/* Merged stats. */
	if (merged && (ninitialized > 1 || !unmerged)) {
		stats_print_one_arena(emitter, MALLCTL_ARENAS_ALL, "merged",
		    "Merged arenas stats:\n", bins, large, mutex, extents, hpa);
	}

	/* Destroyed stats. */
	if (destroyed_initialized && destroyed) {
		stats_print_one_arena(emitter, MALLCTL_ARENAS_DESTROYED,
		    "destroyed", "Destroyed arenas stats:\n", bins, large, mutex,
		    extents, hpa);
	}

	/* Unmerged stats. */
	if (unmerged) {
		for (unsigned i = 0; i < narenas; i++) {
			if (!initialized[i]) {
				continue;
			}
			char arena_ind_str[20];
			malloc_snprintf(arena_ind_str, sizeof(arena_ind_str),
			    "%u", i);
			char header[32];
			malloc_snprintf(header, sizeof(header),
			    "arenas[%s]:\n", arena_ind_str);
			stats_print_one_arena(emitter, i, arena_ind_str, header,
			    bins, large, mutex, extents, hpa);
		}
	}

	emitter_json_object_end(emitter); /* Close "stats.arenas". */
}

static uint64_t
stats_interval_new_event_wait(tsd_t *tsd) {
	return stats_interval_accum_batch;
}

static uint64_t
stats_interval_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

static void
stats_interval_event_handler(tsd_t *tsd) {
	uint64_t last_event = thread_allocated_last_event_get(tsd);
	uint64_t last_sample_event = tsd_stats_interval_last_event_get(tsd);
	tsd_stats_interval_last_event_set(tsd, last_event);
	uint64_t elapsed = last_event - last_sample_event;

	assert(elapsed > 0 && elapsed != TE_INVALID_ELAPSED);
	if (counter_accum(
	        tsd_tsdn(tsd), &stats_interval_accumulated, elapsed)) {
		je_malloc_stats_print(NULL, NULL, opt_stats_interval_opts);
	}
}

static te_enabled_t
stats_interval_enabled(void) {
	return opt_stats_interval >= 0 ? te_enabled_yes : te_enabled_no;
}

te_base_cb_t stats_interval_te_handler = {
    .enabled = &stats_interval_enabled,
    .new_event_wait = &stats_interval_new_event_wait,
    .postponed_event_wait = &stats_interval_postponed_event_wait,
    .event_handler = &stats_interval_event_handler,
};

bool
stats_boot(void) {
	uint64_t stats_interval;
	if (opt_stats_interval < 0) {
		assert(opt_stats_interval == -1);
		stats_interval = 0;
		stats_interval_accum_batch = 0;
	} else {
		/* See comments in stats.h */
		stats_interval = (opt_stats_interval > 0) ? opt_stats_interval
		                                          : 1;
		uint64_t batch = stats_interval
		    >> STATS_INTERVAL_ACCUM_LG_BATCH_SIZE;
		if (batch > STATS_INTERVAL_ACCUM_BATCH_MAX) {
			batch = STATS_INTERVAL_ACCUM_BATCH_MAX;
		} else if (batch == 0) {
			batch = 1;
		}
		stats_interval_accum_batch = batch;
	}

	return counter_accum_init(&stats_interval_accumulated, stats_interval);
}

void
stats_prefork(tsdn_t *tsdn) {
	counter_prefork(tsdn, &stats_interval_accumulated);
}

void
stats_postfork_parent(tsdn_t *tsdn) {
	counter_postfork_parent(tsdn, &stats_interval_accumulated);
}

void
stats_postfork_child(tsdn_t *tsdn) {
	counter_postfork_child(tsdn, &stats_interval_accumulated);
}
