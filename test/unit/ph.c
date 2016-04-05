#include "test/jemalloc_test.h"

typedef struct node_s node_t;

struct node_s {
	ph_node_t link;
};

TEST_BEGIN(test_ph_empty)
{
	ph_heap_t heap;

	ph_new(&heap);

	assert_ptr_null(ph_first(&heap), "Unexpected node");
}
TEST_END

TEST_BEGIN(test_ph_random)
{
#define	NNODES 25
#define	SEED 42
	sfmt_t *sfmt;
	ph_heap_t heap;
	node_t nodes[NNODES];
	unsigned i, j, k;

	sfmt = init_gen_rand(SEED);
	for (i = 0; i < 2; i++) {
		for (j = 1; j <= NNODES; j++) {
			/* Initialize heap and nodes. */
			ph_new(&heap);

			/* Insert nodes. */
			for (k = 0; k < j; k++) {
				ph_insert(&heap, &nodes[k].link);

				assert_ptr_not_null(ph_first(&heap),
				    "Heap should not be empty");
			}

			/* Remove nodes. */
			switch (i % 2) {
			case 0:
				for (k = 0; k < j; k++)
					ph_remove(&heap, &nodes[k].link);
				break;
			case 1:
				for (k = j; k > 0; k--)
					ph_remove(&heap, &nodes[k-1].link);
				break;
			default:
				not_reached();
			}

			assert_ptr_null(ph_first(&heap),
			    "Heap should not be empty");
		}
	}
	fini_gen_rand(sfmt);
#undef NNODES
#undef SEED
}
TEST_END

int
main(void)
{

	return (test(
	    test_ph_empty,
	    test_ph_random));
}
