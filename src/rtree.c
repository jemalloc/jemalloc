#define	JEMALLOC_RTREE_C_
#include "jemalloc/internal/jemalloc_internal.h"

static unsigned
hmin(unsigned ha, unsigned hb)
{

	return (ha < hb ? ha : hb);
}

/*
 * Only the most significant bits of keys passed to rtree_{read,write}() are
 * used.
 */
bool
rtree_new(rtree_t *rtree, unsigned bits)
{
	unsigned bits_in_leaf, height, i;

	assert(RTREE_HEIGHT_MAX == ((ZU(1) << (LG_SIZEOF_PTR+3)) /
	    RTREE_BITS_PER_LEVEL));
	assert(bits > 0 && bits <= (sizeof(uintptr_t) << 3));

	bits_in_leaf = (bits % RTREE_BITS_PER_LEVEL) == 0 ? RTREE_BITS_PER_LEVEL
	    : (bits % RTREE_BITS_PER_LEVEL);
	if (bits > bits_in_leaf) {
		height = 1 + (bits - bits_in_leaf) / RTREE_BITS_PER_LEVEL;
		if ((height-1) * RTREE_BITS_PER_LEVEL + bits_in_leaf != bits)
			height++;
	} else
		height = 1;
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

	return (false);
}

#ifdef JEMALLOC_JET
#undef rtree_node_alloc
#define	rtree_node_alloc JEMALLOC_N(rtree_node_alloc_impl)
#endif
static rtree_elm_t *
rtree_node_alloc(tsdn_t *tsdn, rtree_t *rtree, size_t nelms)
{

	return ((rtree_elm_t *)base_alloc(tsdn, nelms * sizeof(rtree_elm_t)));
}
#ifdef JEMALLOC_JET
#undef rtree_node_alloc
#define	rtree_node_alloc JEMALLOC_N(rtree_node_alloc)
rtree_node_alloc_t *rtree_node_alloc = JEMALLOC_N(rtree_node_alloc_impl);
#endif

#ifdef JEMALLOC_JET
#undef rtree_node_dalloc
#define	rtree_node_dalloc JEMALLOC_N(rtree_node_dalloc_impl)
#endif
UNUSED static void
rtree_node_dalloc(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *node)
{

	/* Nodes are never deleted during normal operation. */
	not_reached();
}
#ifdef JEMALLOC_JET
#undef rtree_node_dalloc
#define	rtree_node_dalloc JEMALLOC_N(rtree_node_dalloc)
rtree_node_dalloc_t *rtree_node_dalloc = JEMALLOC_N(rtree_node_dalloc_impl);
#endif

#ifdef JEMALLOC_JET
static void
rtree_delete_subtree(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *node,
    unsigned level)
{

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
rtree_delete(tsdn_t *tsdn, rtree_t *rtree)
{
	unsigned i;

	for (i = 0; i < rtree->height; i++) {
		rtree_elm_t *subtree = rtree->levels[i].subtree;
		if (subtree != NULL)
			rtree_delete_subtree(tsdn, rtree, subtree, i);
	}
}
#endif

static rtree_elm_t *
rtree_node_init(tsdn_t *tsdn, rtree_t *rtree, unsigned level,
    rtree_elm_t **elmp)
{
	rtree_elm_t *node;

	if (atomic_cas_p((void **)elmp, NULL, RTREE_NODE_INITIALIZING)) {
		spin_t spinner;

		/*
		 * Another thread is already in the process of initializing.
		 * Spin-wait until initialization is complete.
		 */
		spin_init(&spinner);
		do {
			spin_adaptive(&spinner);
			node = atomic_read_p((void **)elmp);
		} while (node == RTREE_NODE_INITIALIZING);
	} else {
		node = rtree_node_alloc(tsdn, rtree, ZU(1) <<
		    rtree->levels[level].bits);
		if (node == NULL)
			return (NULL);
		atomic_write_p((void **)elmp, node);
	}

	return (node);
}

rtree_elm_t *
rtree_subtree_read_hard(tsdn_t *tsdn, rtree_t *rtree, unsigned level)
{

	return (rtree_node_init(tsdn, rtree, level,
	    &rtree->levels[level].subtree));
}

rtree_elm_t *
rtree_child_read_hard(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *elm,
    unsigned level)
{

	return (rtree_node_init(tsdn, rtree, level, &elm->child));
}

static int
rtree_elm_witness_comp(const witness_t *a, void *oa, const witness_t *b,
    void *ob)
{
	uintptr_t ka = (uintptr_t)oa;
	uintptr_t kb = (uintptr_t)ob;

	assert(ka != 0);
	assert(kb != 0);

	return ((ka > kb) - (ka < kb));
}

static witness_t *
rtree_elm_witness_alloc(tsd_t *tsd, uintptr_t key, const rtree_elm_t *elm)
{
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
	return (witness);
}

static witness_t *
rtree_elm_witness_find(tsd_t *tsd, const rtree_elm_t *elm)
{
	size_t i;
	rtree_elm_witness_tsd_t *witnesses = tsd_rtree_elm_witnessesp_get(tsd);

	for (i = 0; i < sizeof(rtree_elm_witness_tsd_t) / sizeof(witness_t);
	    i++) {
		rtree_elm_witness_t *rew = &witnesses->witnesses[i];

		if (rew->elm == elm)
			return (&rew->witness);
	}
	not_reached();
}

static void
rtree_elm_witness_dalloc(tsd_t *tsd, witness_t *witness, const rtree_elm_t *elm)
{
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
    const rtree_elm_t *elm)
{
	witness_t *witness;

	if (tsdn_null(tsdn))
		return;

	witness = rtree_elm_witness_alloc(tsdn_tsd(tsdn), key, elm);
	witness_lock(tsdn, witness);
}

void
rtree_elm_witness_access(tsdn_t *tsdn, const rtree_t *rtree,
    const rtree_elm_t *elm)
{
	witness_t *witness;

	if (tsdn_null(tsdn))
		return;

	witness = rtree_elm_witness_find(tsdn_tsd(tsdn), elm);
	witness_assert_owner(tsdn, witness);
}

void
rtree_elm_witness_release(tsdn_t *tsdn, const rtree_t *rtree,
    const rtree_elm_t *elm)
{
	witness_t *witness;

	if (tsdn_null(tsdn))
		return;

	witness = rtree_elm_witness_find(tsdn_tsd(tsdn), elm);
	witness_unlock(tsdn, witness);
	rtree_elm_witness_dalloc(tsdn_tsd(tsdn), witness, elm);
}
