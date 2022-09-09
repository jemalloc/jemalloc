#include "test/jemalloc_test.h"

#define BT_FRAME(i) {                                           \
        void *p;                                                \
        if (__builtin_frame_address(i) == 0) {                  \
                return;                                         \
        }                                                       \
        p = __builtin_return_address(i);                        \
        if (p == NULL) {                                        \
                return;                                         \
        }                                                       \
}
void
test_outer() {
        BT_FRAME(0)
        BT_FRAME(1)
        BT_FRAME(2)
        BT_FRAME(3)
        BT_FRAME(4)
        BT_FRAME(5)
        BT_FRAME(6)
        BT_FRAME(7)
        BT_FRAME(8)
        BT_FRAME(9)
}

TEST_BEGIN(test_a0) {
	void *p;

	p = a0malloc(1);
	expect_ptr_not_null(p, "Unexpected a0malloc() error");
	a0dalloc(p);
}
TEST_END

TEST_BEGIN(builtin_test) {
	test_skip_if(config_prof_libgcc || config_prof_libunwind);
	test_outer();
}
TEST_END

int
main(void) {
	return test_no_malloc_init(
            builtin_test,
	    test_a0);
}
