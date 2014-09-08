#include "test/jemalloc_test.h"

JEMALLOC_INLINE_C void
time_func(timer_t *timer, uint64_t nwarmup, uint64_t niter, void (*func)(void))
{
	uint64_t i;

	for (i = 0; i < nwarmup; i++)
		func();
	timer_start(timer);
	for (i = 0; i < niter; i++)
		func();
	timer_stop(timer);
}

void
compare_funcs(uint64_t nwarmup, uint64_t niter, const char *name_a,
    void (*func_a), const char *name_b, void (*func_b))
{
	timer_t timer_a, timer_b;
	char ratio_buf[6];

	time_func(&timer_a, nwarmup, niter, func_a);
	time_func(&timer_b, nwarmup, niter, func_b);

	timer_ratio(&timer_a, &timer_b, ratio_buf, sizeof(ratio_buf));
	malloc_printf("%"PRIu64" iterations, %s=%"PRIu64"us, "
	    "%s=%"PRIu64"us, ratio=1:%s\n",
	    niter, name_a, timer_usec(&timer_a), name_b, timer_usec(&timer_b),
	    ratio_buf);
}

static void
malloc_vs_mallocx_malloc(void)
{

	free(malloc(1));
}

static void
malloc_vs_mallocx_mallocx(void)
{

	free(mallocx(1, 0));
}

TEST_BEGIN(test_malloc_vs_mallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "malloc",
	    malloc_vs_mallocx_malloc, "mallocx", malloc_vs_mallocx_mallocx);
}
TEST_END

static void
free_vs_dallocx_free(void)
{

	free(malloc(1));
}

static void
free_vs_dallocx_dallocx(void)
{

	dallocx(malloc(1), 0);
}

TEST_BEGIN(test_free_vs_dallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "free", free_vs_dallocx_free,
	    "dallocx", free_vs_dallocx_dallocx);
}
TEST_END

static void
mus_vs_sallocx_mus(void)
{
	void *p;

	p = malloc(1);
	malloc_usable_size(p);
	free(p);
}

static void
mus_vs_sallocx_sallocx(void)
{
	void *p;

	p = malloc(1);
	sallocx(p, 0);
	free(p);
}

TEST_BEGIN(test_mus_vs_sallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "malloc_usable_size",
	    mus_vs_sallocx_mus, "sallocx", mus_vs_sallocx_sallocx);
}
TEST_END

static void
sallocx_vs_nallocx_sallocx(void)
{
	void *p;

	p = malloc(1);
	sallocx(p, 0);
	free(p);
}

static void
sallocx_vs_nallocx_nallocx(void)
{
	void *p;

	p = malloc(1);
	nallocx(1, 0);
	free(p);
}

TEST_BEGIN(test_sallocx_vs_nallocx)
{

	compare_funcs(10*1000*1000, 100*1000*1000, "sallocx",
	    sallocx_vs_nallocx_sallocx, "nallocx", sallocx_vs_nallocx_nallocx);
}
TEST_END

int
main(void)
{

	return (test(
	    test_malloc_vs_mallocx,
	    test_free_vs_dallocx,
	    test_mus_vs_sallocx,
	    test_sallocx_vs_nallocx));
}
