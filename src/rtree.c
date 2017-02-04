#define JEMALLOC_RTREE_C_
#include "jemalloc/internal/jemalloc_internal.h"

static unsigned
hmin(unsigned ha, unsigned hb) {
	return (ha < hb ? ha : hb);
}

/*
 * Only the most significant bits of keys passed to rtree_{read,write}() are
 * used.
 */
bool
rtree_new(rtree_t *rtree, unsigned bits) {
	unsigned bits_in_leaf, height, i;

	assert(RTREE_HEIGHT_MAX == ((ZU(1) << (LG_SIZEOF_PTR+3)) /
	    RTREE_BITS_PER_LEVEL));
	assert(bits > 0 && bits <= (sizeof(uintptr_t) << 3));

	bits_in_leaf = (bits % RTREE_BITS_PER_LEVEL) == 0 ? RTREE_BITS_PER_LEVEL
	    : (bits % RTREE_BITS_PER_LEVEL);
	if (bits > bits_in_leaf) {
		height = 1 + (bits - bits_in_leaf) / RTREE_BITS_PER_LEVEL;
		if ((height-1) * RTREE_BITS_PER_LEVEL + bits_in_leaf != bits) {
			height++;
		}
	} else {
		height = 1;
	}
	assert((height-1) * RTREE_BITS_PER_LEVEL + bits_in_leaf == bits);

	rtree->height = height;

	/* Root level. */
	rtree->levels[0].subtree = NULL;
	rtree->levels[0].bits = (height > 1) ? RTREE_BITS_PER_LEVEL :
	    bits_in_leaf;
	rtree->levels[0].cumbits = rtree->levels[0].bits;
	/* Interior levels. */
	for (i = 1; i < height-1; i++) {
		rtree->levels[i].subtree = NULL;
		rtree->levels[i].bits = RTREE_BITS_PER_LEVEL;
		rtree->levels[i].cumbits = rtree->levels[i-1].cumbits +
		    RTREE_BITS_PER_LEVEL;
	}
	/* Leaf level. */
	if (height > 1) {
		rtree->levels[height-1].subtree = NULL;
		rtree->levels[height-1].bits = bits_in_leaf;
		rtree->levels[height-1].cumbits = bits;
	}

	/* Compute lookup table to be used by rtree_[ctx_]start_level(). */
	for (i = 0; i < RTREE_HEIGHT_MAX; i++) {
		rtree->start_level[i] = hmin(RTREE_HEIGHT_MAX - 1 - i, height -
		    1);
	}
	rtree->start_level[RTREE_HEIGHT_MAX] = 0;

	malloc_mutex_init(&rtree->init_lock, "rtree", WITNESS_RANK_RTREE);

	return false;
}

#ifdef JEMALLOC_JET
#undef rtree_node_alloc
#define rtree_node_alloc JEMALLOC_N(rtree_node_alloc_impl)
#endif
static rtree_elm_t *
rtree_node_alloc(tsdn_t *tsdn, rtree_t *rtree, size_t nelms) {
	return (rtree_elm_t *)base_alloc(tsdn, b0get(), nelms *
	    sizeof(rtree_elm_t), CACHELINE);
}
#ifdef JEMALLOC_JET
#undef rtree_node_alloc
#define rtree_node_alloc JEMALLOC_N(rtree_node_alloc)
rtree_node_alloc_t *rtree_node_alloc = JEMALLOC_N(rtree_node_alloc_impl);
#endif

#ifdef JEMALLOC_JET
#undef rtree_node_dalloc
#define rtree_node_dalloc JEMALLOC_N(rtree_node_dalloc_impl)
#endif
UNUSED static void
rtree_node_dalloc(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *node) {
	/* Nodes are never deleted during normal operation. */
	not_reached();
}
#ifdef JEMALLOC_JET
#undef rtree_node_dalloc
#define rtree_node_dalloc JEMALLOC_N(rtree_node_dalloc)
rtree_node_dalloc_t *rtree_node_dalloc = JEMALLOC_N(rtree_node_dalloc_impl);
#endif

#ifdef JEMALLOC_JET
static void
rtree_delete_subtree(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *node,
    unsigned level) {
	if (level + 1 < rtree->height) {
		size_t nchildren, i;

		nchildren = ZU(1) << rtree->levels[level].bits;
		for (i = 0; i < nchildren; i++) {
			rtree_elm_t *child = node[i].child;
			if (child != NULL) {
				rtree_delete_subtree(tsdn, rtree, child, level +
				    1);
			}
		}
	}
	rtree_node_dalloc(tsdn, rtree, node);
}

void
rtree_delete(tsdn_t *tsdn, rtree_t *rtree) {
	unsigned i;

	for (i = 0; i < rtree->height; i++) {
		rtree_elm_t *subtree = rtree->levels[i].subtree;
		if (subtree != NULL) {
			rtree_delete_subtree(tsdn, rtree, subtree, i);
		}
	}
}
#endif

static rtree_elm_t *
rtree_node_init(tsdn_t *tsdn, rtree_t *rtree, unsigned level,
    rtree_elm_t **elmp) {
	rtree_elm_t *node;

	malloc_mutex_lock(tsdn, &rtree->init_lock);
	node = atomic_read_p((void**)elmp);
	if (node == NULL) {
		node = rtree_node_alloc(tsdn, rtree, ZU(1) <<
		    rtree->levels[level].bits);
		if (node == NULL) {
			malloc_mutex_unlock(tsdn, &rtree->init_lock);
			return NULL;
		}
		atomic_write_p((void **)elmp, node);
	}
	malloc_mutex_unlock(tsdn, &rtree->init_lock);

	return node;
}

static unsigned
rtree_start_level(const rtree_t *rtree, uintptr_t key) {
	unsigned start_level;

	if (unlikely(key == 0)) {
		return rtree->height - 1;
	}

	start_level = rtree->start_level[(lg_floor(key) + 1) >>
	    LG_RTREE_BITS_PER_LEVEL];
	assert(start_level < rtree->height);
	return start_level;
}

static uintptr_t
rtree_subkey(rtree_t *rtree, uintptr_t key, unsigned level) {
	return ((key >> ((ZU(1) << (LG_SIZEOF_PTR+3)) -
	    rtree->levels[level].cumbits)) & ((ZU(1) <<
	    rtree->levels[level].bits) - 1));
}

static bool
rtree_node_valid(rtree_elm_t *node) {
	return ((uintptr_t)node != (uintptr_t)0);
}

static rtree_elm_t *
rtree_child_tryread(rtree_elm_t *elm, bool dependent) {
	rtree_elm_t *child;

	/* Double-checked read (first read may be stale). */
	child = elm->child;
	if (!dependent && !rtree_node_valid(child)) {
		child = (rtree_elm_t *)atomic_read_p(&elm->pun);
	}
	assert(!dependent || child != NULL);
	return child;
}

static rtree_elm_t *
rtree_child_read(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *elm, unsigned level,
    bool dependent) {
	rtree_elm_t *child;

	child = rtree_child_tryread(elm, dependent);
	if (!dependent && unlikely(!rtree_node_valid(child))) {
		child = rtree_node_init(tsdn, rtree, level+1, &elm->child);
	}
	assert(!dependent || child != NULL);
	return child;
}

static rtree_elm_t *
rtree_subtree_tryread(rtree_t *rtree, unsigned level, bool dependent) {
	rtree_elm_t *subtree;

	/* Double-checked read (first read may be stale). */
	subtree = rtree->levels[level].subtree;
	if (!dependent && unlikely(!rtree_node_valid(subtree))) {
		subtree = (rtree_elm_t *)atomic_read_p(
		    &rtree->levels[level].subtree_pun);
	}
	assert(!dependent || subtree != NULL);
	return subtree;
}

static rtree_elm_t *
rtree_subtree_read(tsdn_t *tsdn, rtree_t *rtree, unsigned level,
    bool dependent) {
	rtree_elm_t *subtree;

	subtree = rtree_subtree_tryread(rtree, level, dependent);
	if (!dependent && unlikely(!rtree_node_valid(subtree))) {
		subtree = rtree_node_init(tsdn, rtree, level,
		    &rtree->levels[level].subtree);
	}
	assert(!dependent || subtree != NULL);
	return subtree;
}

rtree_elm_t *
rtree_elm_lookup_hard(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, bool init_missing) {
	/*
	 * Search the remaining cache elements, and on success move the matching
	 * element to the front.
	 */
	if (likely(key != 0)) {
		for (unsigned i = 1; i < RTREE_CTX_NCACHE; i++) {
			if (rtree_ctx->cache[i].key == key) {
				/* Reorder. */
				rtree_elm_t *elm = rtree_ctx->cache[i].elm;
				memmove(&rtree_ctx->cache[1],
				    &rtree_ctx->cache[0],
				    sizeof(rtree_ctx_cache_elm_t) * i);
				rtree_ctx->cache[0].key = key;
				rtree_ctx->cache[0].elm = elm;

				return elm;
			}
		}
	}

	unsigned start_level = rtree_start_level(rtree, key);
	rtree_elm_t *node = init_missing ? rtree_subtree_read(tsdn, rtree,
	    start_level, dependent) : rtree_subtree_tryread(rtree, start_level,
	    dependent);

#define RTREE_GET_BIAS	(RTREE_HEIGHT_MAX - rtree->height)
	switch (start_level + RTREE_GET_BIAS) {
#define RTREE_GET_SUBTREE(level)					\
	case level: {							\
		assert(level < (RTREE_HEIGHT_MAX-1));			\
		if (!dependent && unlikely(!rtree_node_valid(node))) {	\
			return NULL;					\
		}							\
		uintptr_t subkey = rtree_subkey(rtree, key, level -	\
		    RTREE_GET_BIAS);					\
		node = init_missing ? rtree_child_read(tsdn, rtree,	\
		    &node[subkey], level - RTREE_GET_BIAS, dependent) :	\
		    rtree_child_tryread(&node[subkey], dependent);	\
		/* Fall through. */					\
	}
#define RTREE_GET_LEAF(level)						\
	case level: {							\
		assert(level == (RTREE_HEIGHT_MAX-1));			\
		if (!dependent && unlikely(!rtree_node_valid(node))) {	\
			return NULL;					\
		}							\
		uintptr_t subkey = rtree_subkey(rtree, key, level -	\
		    RTREE_GET_BIAS);					\
		/*							\
		 * node is a leaf, so it contains values rather than	\
		 * child pointers.					\
		 */							\
		node = &node[subkey];					\
		if (likely(key != 0)) {					\
			if (RTREE_CTX_NCACHE > 1) {			\
				memmove(&rtree_ctx->cache[1],		\
				    &rtree_ctx->cache[0],		\
				    sizeof(rtree_ctx_cache_elm_t) *	\
				    (RTREE_CTX_NCACHE-1));		\
			}						\
			rtree_ctx->cache[0].key = key;			\
			rtree_ctx->cache[0].elm = node;			\
		}							\
		return node;						\
	}
#if RTREE_HEIGHT_MAX > 1
	RTREE_GET_SUBTREE(0)
#endif
#if RTREE_HEIGHT_MAX > 2
	RTREE_GET_SUBTREE(1)
#endif
#if RTREE_HEIGHT_MAX > 3
	RTREE_GET_SUBTREE(2)
#endif
#if RTREE_HEIGHT_MAX > 4
	RTREE_GET_SUBTREE(3)
#endif
#if RTREE_HEIGHT_MAX > 5
	RTREE_GET_SUBTREE(4)
#endif
#if RTREE_HEIGHT_MAX > 6
	RTREE_GET_SUBTREE(5)
#endif
#if RTREE_HEIGHT_MAX > 7
	RTREE_GET_SUBTREE(6)
#endif
#if RTREE_HEIGHT_MAX > 8
	RTREE_GET_SUBTREE(7)
#endif
#if RTREE_HEIGHT_MAX > 9
	RTREE_GET_SUBTREE(8)
#endif
#if RTREE_HEIGHT_MAX > 10
	RTREE_GET_SUBTREE(9)
#endif
#if RTREE_HEIGHT_MAX > 11
	RTREE_GET_SUBTREE(10)
#endif
#if RTREE_HEIGHT_MAX > 12
	RTREE_GET_SUBTREE(11)
#endif
#if RTREE_HEIGHT_MAX > 13
	RTREE_GET_SUBTREE(12)
#endif
#if RTREE_HEIGHT_MAX > 14
	RTREE_GET_SUBTREE(13)
#endif
#if RTREE_HEIGHT_MAX > 15
	RTREE_GET_SUBTREE(14)
#endif
#if RTREE_HEIGHT_MAX > 16
#  error Unsupported RTREE_HEIGHT_MAX
#endif
	RTREE_GET_LEAF(RTREE_HEIGHT_MAX-1)
#undef RTREE_GET_SUBTREE
#undef RTREE_GET_LEAF
	default: not_reached();
	}
#undef RTREE_GET_BIAS
	not_reached();
}

static int
rtree_elm_witness_comp(const witness_t *a, void *oa, const witness_t *b,
    void *ob) {
	uintptr_t ka = (uintptr_t)oa;
	uintptr_t kb = (uintptr_t)ob;

	assert(ka != 0);
	assert(kb != 0);

	return (ka > kb) - (ka < kb);
}

static witness_t *
rtree_elm_witness_alloc(tsd_t *tsd, uintptr_t key, const rtree_elm_t *elm) {
	witness_t *witness;
	size_t i;
	rtree_elm_witness_tsd_t *witnesses = tsd_rtree_elm_witnessesp_get(tsd);

	/* Iterate over entire array to detect double allocation attempts. */
	witness = NULL;
	for (i = 0; i < sizeof(rtree_elm_witness_tsd_t) / sizeof(witness_t);
	    i++) {
		rtree_elm_witness_t *rew = &witnesses->witnesses[i];

		assert(rew->elm != elm);
		if (rew->elm == NULL && witness == NULL) {
			rew->elm = elm;
			witness = &rew->witness;
			witness_init(witness, "rtree_elm",
			    WITNESS_RANK_RTREE_ELM, rtree_elm_witness_comp,
			    (void *)key);
		}
	}
	assert(witness != NULL);
	return witness;
}

static witness_t *
rtree_elm_witness_find(tsd_t *tsd, const rtree_elm_t *elm) {
	size_t i;
	rtree_elm_witness_tsd_t *witnesses = tsd_rtree_elm_witnessesp_get(tsd);

	for (i = 0; i < sizeof(rtree_elm_witness_tsd_t) / sizeof(witness_t);
	    i++) {
		rtree_elm_witness_t *rew = &witnesses->witnesses[i];

		if (rew->elm == elm) {
			return &rew->witness;
		}
	}
	not_reached();
}

static void
rtree_elm_witness_dalloc(tsd_t *tsd, witness_t *witness,
    const rtree_elm_t *elm) {
	size_t i;
	rtree_elm_witness_tsd_t *witnesses = tsd_rtree_elm_witnessesp_get(tsd);

	for (i = 0; i < sizeof(rtree_elm_witness_tsd_t) / sizeof(witness_t);
	    i++) {
		rtree_elm_witness_t *rew = &witnesses->witnesses[i];

		if (rew->elm == elm) {
			rew->elm = NULL;
			witness_init(&rew->witness, "rtree_elm",
			    WITNESS_RANK_RTREE_ELM, rtree_elm_witness_comp,
			    NULL);
			    return;
		}
	}
	not_reached();
}

void
rtree_elm_witness_acquire(tsdn_t *tsdn, const rtree_t *rtree, uintptr_t key,
    const rtree_elm_t *elm) {
	witness_t *witness;

	if (tsdn_null(tsdn)) {
		return;
	}

	witness = rtree_elm_witness_alloc(tsdn_tsd(tsdn), key, elm);
	witness_lock(tsdn, witness);
}

void
rtree_elm_witness_access(tsdn_t *tsdn, const rtree_t *rtree,
    const rtree_elm_t *elm) {
	witness_t *witness;

	if (tsdn_null(tsdn)) {
		return;
	}

	witness = rtree_elm_witness_find(tsdn_tsd(tsdn), elm);
	witness_assert_owner(tsdn, witness);
}

void
rtree_elm_witness_release(tsdn_t *tsdn, const rtree_t *rtree,
    const rtree_elm_t *elm) {
	witness_t *witness;

	if (tsdn_null(tsdn)) {
		return;
	}

	witness = rtree_elm_witness_find(tsdn_tsd(tsdn), elm);
	witness_unlock(tsdn, witness);
	rtree_elm_witness_dalloc(tsdn_tsd(tsdn), witness, elm);
}
