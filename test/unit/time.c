#include "test/jemalloc_test.h"

#define	BILLION	1000000000

TEST_BEGIN(test_time_init)
{
	struct timespec ts;

	time_init(&ts, 42, 43);
	assert_ld_eq(ts.tv_sec, 42, "tv_sec incorrectly initialized");
	assert_ld_eq(ts.tv_nsec, 43, "tv_nsec incorrectly initialized");
}
TEST_END

TEST_BEGIN(test_time_sec)
{
	struct timespec ts;

	time_init(&ts, 42, 43);
	assert_ld_eq(time_sec(&ts), 42, "tv_sec incorrectly read");
}
TEST_END

TEST_BEGIN(test_time_nsec)
{
	struct timespec ts;

	time_init(&ts, 42, 43);
	assert_ld_eq(time_nsec(&ts), 43, "tv_nsec incorrectly read");
}
TEST_END

TEST_BEGIN(test_time_copy)
{
	struct timespec tsa, tsb;

	time_init(&tsa, 42, 43);
	time_init(&tsb, 0, 0);
	time_copy(&tsb, &tsa);
	assert_ld_eq(time_sec(&tsb), 42, "tv_sec incorrectly copied");
	assert_ld_eq(time_nsec(&tsb), 43, "tv_nsec incorrectly copied");
}
TEST_END

TEST_BEGIN(test_time_compare)
{
	struct timespec tsa, tsb;

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	assert_d_eq(time_compare(&tsa, &tsb), 0, "Times should be equal");
	assert_d_eq(time_compare(&tsb, &tsa), 0, "Times should be equal");

	time_init(&tsb, 42, 42);
	assert_d_eq(time_compare(&tsa, &tsb), 1,
	    "tsa should be greater than tsb");
	assert_d_eq(time_compare(&tsb, &tsa), -1,
	    "tsb should be less than tsa");

	time_init(&tsb, 42, 44);
	assert_d_eq(time_compare(&tsa, &tsb), -1,
	    "tsa should be less than tsb");
	assert_d_eq(time_compare(&tsb, &tsa), 1,
	    "tsb should be greater than tsa");

	time_init(&tsb, 41, BILLION - 1);
	assert_d_eq(time_compare(&tsa, &tsb), 1,
	    "tsa should be greater than tsb");
	assert_d_eq(time_compare(&tsb, &tsa), -1,
	    "tsb should be less than tsa");

	time_init(&tsb, 43, 0);
	assert_d_eq(time_compare(&tsa, &tsb), -1,
	    "tsa should be less than tsb");
	assert_d_eq(time_compare(&tsb, &tsa), 1,
	    "tsb should be greater than tsa");
}
TEST_END

TEST_BEGIN(test_time_add)
{
	struct timespec tsa, tsb;

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	time_add(&tsa, &tsb);
	time_init(&tsb, 84, 86);
	assert_d_eq(time_compare(&tsa, &tsb), 0, "Incorrect addition result");

	time_init(&tsa, 42, BILLION - 1);
	time_copy(&tsb, &tsa);
	time_add(&tsa, &tsb);
	time_init(&tsb, 85, BILLION - 2);
	assert_d_eq(time_compare(&tsa, &tsb), 0, "Incorrect addition result");
}
TEST_END

TEST_BEGIN(test_time_subtract)
{
	struct timespec tsa, tsb;

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	time_subtract(&tsa, &tsb);
	time_init(&tsb, 0, 0);
	assert_d_eq(time_compare(&tsa, &tsb), 0,
	    "Incorrect subtraction result");

	time_init(&tsa, 42, 43);
	time_init(&tsb, 41, 44);
	time_subtract(&tsa, &tsb);
	time_init(&tsb, 0, BILLION - 1);
	assert_d_eq(time_compare(&tsa, &tsb), 0,
	    "Incorrect subtraction result");
}
TEST_END

TEST_BEGIN(test_time_imultiply)
{
	struct timespec tsa, tsb;

	time_init(&tsa, 42, 43);
	time_imultiply(&tsa, 10);
	time_init(&tsb, 420, 430);
	assert_d_eq(time_compare(&tsa, &tsb), 0,
	    "Incorrect multiplication result");

	time_init(&tsa, 42, 666666666);
	time_imultiply(&tsa, 3);
	time_init(&tsb, 127, 999999998);
	assert_d_eq(time_compare(&tsa, &tsb), 0,
	    "Incorrect multiplication result");
}
TEST_END

TEST_BEGIN(test_time_idivide)
{
	struct timespec tsa, tsb;

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	time_imultiply(&tsa, 10);
	time_idivide(&tsa, 10);
	assert_d_eq(time_compare(&tsa, &tsb), 0, "Incorrect division result");

	time_init(&tsa, 42, 666666666);
	time_copy(&tsb, &tsa);
	time_imultiply(&tsa, 3);
	time_idivide(&tsa, 3);
	assert_d_eq(time_compare(&tsa, &tsb), 0, "Incorrect division result");
}
TEST_END

TEST_BEGIN(test_time_divide)
{
	struct timespec tsa, tsb, tsc;

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	time_imultiply(&tsa, 10);
	assert_u64_eq(time_divide(&tsa, &tsb), 10,
	    "Incorrect division result");

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	time_imultiply(&tsa, 10);
	time_init(&tsc, 0, 1);
	time_add(&tsa, &tsc);
	assert_u64_eq(time_divide(&tsa, &tsb), 10,
	    "Incorrect division result");

	time_init(&tsa, 42, 43);
	time_copy(&tsb, &tsa);
	time_imultiply(&tsa, 10);
	time_init(&tsc, 0, 1);
	time_subtract(&tsa, &tsc);
	assert_u64_eq(time_divide(&tsa, &tsb), 9, "Incorrect division result");
}
TEST_END

TEST_BEGIN(test_time_update)
{
	struct timespec ts;

	time_init(&ts, 0, 0);

	assert_false(time_update(&ts), "Basic time update failed.");

	/* Only Rip Van Winkle sleeps this long. */
	{
		struct timespec addend;
		time_init(&addend, 631152000, 0);
		time_add(&ts, &addend);
	}
	{
		struct timespec ts0;
		time_copy(&ts0, &ts);
		assert_true(time_update(&ts),
		    "Update should detect time roll-back.");
		assert_d_eq(time_compare(&ts, &ts0), 0,
		    "Time should not have been modified");
	}

}
TEST_END

int
main(void)
{

	return (test(
	    test_time_init,
	    test_time_sec,
	    test_time_nsec,
	    test_time_copy,
	    test_time_compare,
	    test_time_add,
	    test_time_subtract,
	    test_time_imultiply,
	    test_time_idivide,
	    test_time_divide,
	    test_time_update));
}
