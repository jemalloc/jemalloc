static inline void
time_func(timedelta_t *timer, uint64_t nwarmup, uint64_t niter,
    void (*func)(void)) {
	uint64_t i;

	for (i = 0; i < nwarmup; i++) {
		func();
	}
	timer_start(timer);
	for (i = 0; i < niter; i++) {
		func();
	}
	timer_stop(timer);
}

static inline void
compare_funcs(uint64_t nwarmup, uint64_t niter, const char *name_a,
    void (*func_a), const char *name_b, void (*func_b)) {
	timedelta_t timer_a, timer_b;
	char ratio_buf[6];
	void *p;

	p = mallocx(1, 0);
	if (p == NULL) {
		test_fail("Unexpected mallocx() failure");
		return;
	}

	time_func(&timer_a, nwarmup, niter, func_a);
	time_func(&timer_b, nwarmup, niter, func_b);

	timer_ratio(&timer_a, &timer_b, ratio_buf, sizeof(ratio_buf));
	malloc_printf("%"FMTu64" iterations, %s=%"FMTu64"us, "
	    "%s=%"FMTu64"us, ratio=1:%s\n",
	    niter, name_a, timer_usec(&timer_a), name_b, timer_usec(&timer_b),
	    ratio_buf);

	dallocx(p, 0);
}
