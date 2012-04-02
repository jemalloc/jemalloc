#include <stdio.h>
#include <assert.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

int
main(void)
{

	fprintf(stderr, "Test begin\n");

	free(malloc(1));
	free(NULL);
	assert(malloc_usable_size(NULL) == 0);

	fprintf(stderr, "Test end\n");
	return (0);
}
