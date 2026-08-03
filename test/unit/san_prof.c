#include "test/jemalloc_test.h"
#include "test/san.h"

#include "jemalloc/internal/prof_inlines.h"

TEST_BEGIN(test_prof_sampled_small_is_not_guarded_as_large) {
	test_skip_if(!config_prof);

	tsd_t *tsd = tsd_fetch();
	void *small = malloc(PAGE / 2);
	expect_ptr_not_null(small, "Unexpected small allocation failure");
	expect_true(prof_sampled(tsd, small),
	    "Expected every allocation to be prof sampled");
	expect_false(extent_is_guarded(tsd_tsdn(tsd), small),
	    "A prof-promoted small allocation is not a large extent");
	free(small);

	void *large = malloc(SC_LARGE_MINCLASS);
	expect_ptr_not_null(large, "Unexpected large allocation failure");
	expect_true(extent_is_guarded(tsd_tsdn(tsd), large),
	    "The next eligible large extent should still be guarded");
	free(large);
}
TEST_END

int
main(void) {
	return test(test_prof_sampled_small_is_not_guarded_as_large);
}
