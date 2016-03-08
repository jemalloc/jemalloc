/*
 * A Pairing Heap implementation.
 *
 * "The Pairing Heap: A New Form of Self-Adjusting Heap"
 * https://www.cs.cmu.edu/~sleator/papers/pairing-heaps.pdf
 *
 * With auxiliary list, described in a follow on paper
 *
 * "Pairing Heaps: Experiments and Analysis"
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.106.2988&rep=rep1&type=pdf
 *
 * Where search/nsearch/last are not needed, ph.h outperforms rb.h by ~7x fewer
 * cpu cycles, and ~4x fewer memory references.
 *
 * Tagging parent/prev pointers on the next list was also described in the
 * original paper, such that only two pointers are needed.  This is not
 * implemented here, as it substantially increases the memory references
 * needed when ph_remove is called, almost overshadowing the other performance
 * gains.
 *
 *******************************************************************************
 */
#ifdef JEMALLOC_H_TYPES

typedef struct ph_node_s ph_node_t;
typedef struct ph_heap_s ph_heap_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct ph_node_s {
	ph_node_t	*subheaps;
	ph_node_t	*parent;
	ph_node_t	*next;
	ph_node_t	*prev;
};

struct ph_heap_s {
	ph_node_t	*root;
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
ph_node_t	*ph_merge_ordered(ph_node_t *heap1, ph_node_t *heap2);
ph_node_t	*ph_merge(ph_node_t *heap1, ph_node_t *heap2);
ph_node_t	*ph_merge_pairs(ph_node_t *subheaps);
void	ph_merge_aux_list(ph_heap_t *l);
void	ph_new(ph_heap_t *n);
ph_node_t	*ph_first(ph_heap_t *l);
void	ph_insert(ph_heap_t *l, ph_node_t *n);
ph_node_t	*ph_remove_first(ph_heap_t *l);
void	ph_remove(ph_heap_t *l, ph_node_t *n);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_PH_C_))

/* Helper routines ************************************************************/

JEMALLOC_INLINE ph_node_t *
ph_merge_ordered(ph_node_t *heap1, ph_node_t *heap2)
{

	assert(heap1 != NULL);
	assert(heap2 != NULL);
	assert ((uintptr_t)heap1 <= (uintptr_t)heap2);

	heap2->parent = heap1;
	heap2->prev = NULL;
	heap2->next = heap1->subheaps;
	if (heap1->subheaps != NULL)
		heap1->subheaps->prev = heap2;
	heap1->subheaps = heap2;
	return (heap1);
}

JEMALLOC_INLINE ph_node_t *
ph_merge(ph_node_t *heap1, ph_node_t *heap2)
{

	if (heap1 == NULL)
		return (heap2);
	if (heap2 == NULL)
		return (heap1);
	/* Optional: user-settable comparison function */
	if ((uintptr_t)heap1 < (uintptr_t)heap2)
		return (ph_merge_ordered(heap1, heap2));
	else
		return (ph_merge_ordered(heap2, heap1));
}

JEMALLOC_INLINE ph_node_t *
ph_merge_pairs(ph_node_t *subheaps)
{

	if (subheaps == NULL)
		return (NULL);
	if (subheaps->next == NULL)
		return (subheaps);
	{
		ph_node_t *l0 = subheaps;
		ph_node_t *l1 = l0->next;
		ph_node_t *lrest = l1->next;

		if (lrest != NULL)
			lrest->prev = NULL;
		l1->next = NULL;
		l1->prev = NULL;
		l0->next = NULL;
		l0->prev = NULL;
		return (ph_merge(ph_merge(l0, l1), ph_merge_pairs(lrest)));
	}
}

/*
 * Merge the aux list into the root node.
 */
JEMALLOC_INLINE void
ph_merge_aux_list(ph_heap_t *l)
{

	if (l->root == NULL)
		return;
	if (l->root->next != NULL) {
		ph_node_t *l0 = l->root->next;
		ph_node_t *l1 = l0->next;
		ph_node_t *lrest = NULL;

		/* Multipass merge. */
		while (l1 != NULL) {
			lrest = l1->next;
			if (lrest != NULL)
				lrest->prev = NULL;
			l1->next = NULL;
			l1->prev = NULL;
			l0->next = NULL;
			l0->prev = NULL;
			l0 = ph_merge(l0, l1);
			l1 = lrest;
		}
		l->root->next = NULL;
		l->root = ph_merge(l->root, l0);
	}
}

/* User API *******************************************************************/

JEMALLOC_INLINE void
ph_new(ph_heap_t *n)
{

	memset(n, 0, sizeof(ph_heap_t));
}

JEMALLOC_INLINE ph_node_t *
ph_first(ph_heap_t *l)
{

	/*
	 * For the cost of an extra pointer, a l->min could be stored instead of
	 * merging the aux list here.  Current users always call ph_remove(l,
	 * ph_first(l)) though, and the aux list must always be merged for
	 * delete of the min node anyway.
	 */
	ph_merge_aux_list(l);
	return (l->root);
}

JEMALLOC_INLINE void
ph_insert(ph_heap_t *l, ph_node_t *n)
{

	memset(n, 0, sizeof(ph_node_t));

	/*
	 * Non-aux list insert:
	 *
	 * l->root = ph_merge(l->root, n);
	 *
	 * Aux list insert:
	 */
	if (l->root == NULL)
		l->root = n;
	else {
		n->next = l->root->next;
		if (l->root->next != NULL)
			l->root->next->prev = n;
		n->prev = l->root;
		l->root->next = n;
	}
}

JEMALLOC_INLINE ph_node_t *
ph_remove_first(ph_heap_t *l)
{
	ph_node_t *ret;

	ph_merge_aux_list(l);
	if (l->root == NULL)
		return (NULL);

	ret = l->root;

	l->root = ph_merge_pairs(l->root->subheaps);

	return (ret);
}

JEMALLOC_INLINE void
ph_remove(ph_heap_t *l, ph_node_t *n)
{
	ph_node_t *replace;

	/*
	 * We can delete from aux list without merging it, but we need to merge
	 * if we are dealing with the root node.
	 */
	if (l->root == n) {
		ph_merge_aux_list(l);
		if (l->root == n) {
			ph_remove_first(l);
			return;
		}
	}

	/* Find a possible replacement node, and link to parent. */
	replace = ph_merge_pairs(n->subheaps);
	if (n->parent != NULL && n->parent->subheaps == n) {
		if (replace != NULL)
			n->parent->subheaps = replace;
		else
			n->parent->subheaps = n->next;
	}
	/* Set next/prev for sibling linked list. */
	if (replace != NULL) {
		replace->parent = n->parent;
		replace->prev = n->prev;
		if (n->prev != NULL)
			n->prev->next = replace;
		replace->next = n->next;
		if (n->next != NULL)
			n->next->prev = replace;
	} else {
		if (n->prev != NULL)
			n->prev->next = n->next;
		if (n->next != NULL)
			n->next->prev = n->prev;
	}
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
