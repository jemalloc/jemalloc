#include "test/jemalloc_test.h"

static const uint64_t interval = 1 << 20;

TEST_BEGIN(test_counter_accum) {
	uint64_t increment = interval >> 4;
	unsigned n = interval / increment;
	uint64_t accum = 0;

	counter_accum_t c;
	counter_accum_init(&c, interval);

	tsd_t *tsd = tsd_fetch();
	bool trigger;
	for (unsigned i = 0; i < n; i++) {
		trigger = counter_accum(tsd_tsdn(tsd), &c, increment);
		accum += increment;
		if (accum < interval) {
			assert_b_eq(trigger, false, "Should not trigger");
		} else {
			assert_b_eq(trigger, true, "Should have triggered");
		}
	}
	assert_b_eq(trigger, true, "Should have triggered");
}
TEST_END

void
assert_counter_value(counter_accum_t *c, uint64_t v) {
	uint64_t accum;
#ifdef JEMALLOC_ATOMIC_U64
	accum = atomic_load_u64(&(c->accumbytes), ATOMIC_RELAXED);
#else
	accum = c->accumbytes;
#endif
	assert_u64_eq(accum, v, "Counter value mismatch");
}

TEST_BEGIN(test_counter_rollback) {
	uint64_t half_interval = interval / 2;

	counter_accum_t c;
	counter_accum_init(&c, interval);

	tsd_t *tsd = tsd_fetch();
	counter_rollback(tsd_tsdn(tsd), &c, half_interval);

	bool trigger;
	trigger = counter_accum(tsd_tsdn(tsd), &c, half_interval);
	assert_b_eq(trigger, false, "Should not trigger");
	counter_rollback(tsd_tsdn(tsd), &c, half_interval + 1);
	assert_counter_value(&c,  0);

	trigger = counter_accum(tsd_tsdn(tsd), &c, half_interval);
	assert_b_eq(trigger, false, "Should not trigger");
	counter_rollback(tsd_tsdn(tsd), &c, half_interval - 1);
	assert_counter_value(&c,  1);

	counter_rollback(tsd_tsdn(tsd), &c, 1);
	assert_counter_value(&c,  0);

	trigger = counter_accum(tsd_tsdn(tsd), &c, half_interval);
	assert_b_eq(trigger, false, "Should not trigger");
	counter_rollback(tsd_tsdn(tsd), &c, 1);
	assert_counter_value(&c,  half_interval - 1);

	trigger = counter_accum(tsd_tsdn(tsd), &c, half_interval);
	assert_b_eq(trigger, false, "Should not trigger");
	assert_counter_value(&c,  interval - 1);

	trigger = counter_accum(tsd_tsdn(tsd), &c, 1);
	assert_b_eq(trigger, true, "Should have triggered");
	assert_counter_value(&c, 0);

	trigger = counter_accum(tsd_tsdn(tsd), &c, interval + 1);
	assert_b_eq(trigger, true, "Should have triggered");
	assert_counter_value(&c, 1);
}
TEST_END

#define N_THDS (16)
#define N_ITER_THD (1 << 12)
#define ITER_INCREMENT (interval >> 4)

static void *
thd_start(void *varg) {
	counter_accum_t *c = (counter_accum_t *)varg;

	tsd_t *tsd = tsd_fetch();
	bool trigger;
	uintptr_t n_triggered = 0;
	for (unsigned i = 0; i < N_ITER_THD; i++) {
		trigger = counter_accum(tsd_tsdn(tsd), c, ITER_INCREMENT);
		n_triggered += trigger ? 1 : 0;
	}

	return (void *)n_triggered;
}


TEST_BEGIN(test_counter_mt) {
	counter_accum_t shared_c;
	counter_accum_init(&shared_c, interval);

	thd_t thds[N_THDS];
	unsigned i;
	for (i = 0; i < N_THDS; i++) {
		thd_create(&thds[i], thd_start, (void *)&shared_c);
	}

	uint64_t sum = 0;
	for (i = 0; i < N_THDS; i++) {
		void *ret;
		thd_join(thds[i], &ret);
		sum += (uintptr_t)ret;
	}
	assert_u64_eq(sum, N_THDS * N_ITER_THD / (interval / ITER_INCREMENT),
	    "Incorrect number of triggers");
}
TEST_END

int
main(void) {
	return test(
	    test_counter_accum,
	    test_counter_rollback,
	    test_counter_mt);
}
