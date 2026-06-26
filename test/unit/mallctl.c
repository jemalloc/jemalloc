#include "test/jemalloc_test.h"

#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/util.h"

extern int ctl_mib_unsigned(
    unsigned *dst, const size_t *mib, size_t mib_index);
extern int ctl_verify_read(void *oldp, size_t *oldlenp,
    size_t expected_size);
extern int ctl_readonly(const void *newp, size_t newlen);
extern int ctl_neither_read_nor_write(void *oldp, size_t *oldlenp,
    const void *newp, size_t newlen);
extern int ctl_read_xor_write(void *oldp, size_t *oldlenp, const void *newp,
    size_t newlen);

TEST_BEGIN(test_mallctl_errors) {
	uint64_t epoch;
	size_t   sz;

	expect_d_eq(mallctl("no_such_name", NULL, NULL, NULL, 0), ENOENT,
	    "mallctl() should return ENOENT for non-existent names");

	expect_d_eq(mallctl("version", NULL, NULL, "0.0.0", strlen("0.0.0")),
	    EPERM,
	    "mallctl() should return EPERM on attempt to write "
	    "read-only value");

	expect_d_eq(
	    mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch) - 1),
	    EINVAL, "mallctl() should return EINVAL for input size mismatch");
	expect_d_eq(
	    mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch) + 1),
	    EINVAL, "mallctl() should return EINVAL for input size mismatch");

	sz = sizeof(epoch) - 1;
	expect_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), EINVAL,
	    "mallctl() should return EINVAL for output size mismatch");
	sz = sizeof(epoch) + 1;
	expect_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), EINVAL,
	    "mallctl() should return EINVAL for output size mismatch");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_errors) {
	size_t mib[1];
	size_t miblen;

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("no_such_name", mib, &miblen), ENOENT,
	    "mallctlnametomib() should return ENOENT for non-existent names");
}
TEST_END

TEST_BEGIN(test_mallctlbymib_errors) {
	uint64_t epoch;
	size_t   sz;
	size_t   mib[1];
	size_t   miblen;

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("version", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");

	expect_d_eq(
	    mallctlbymib(mib, miblen, NULL, NULL, "0.0.0", strlen("0.0.0")),
	    EPERM,
	    "mallctl() should return EPERM on "
	    "attempt to write read-only value");

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("epoch", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");

	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&epoch,
	                sizeof(epoch) - 1),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for input size mismatch");
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&epoch,
	                sizeof(epoch) + 1),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for input size mismatch");

	sz = sizeof(epoch) - 1;
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&epoch, &sz, NULL, 0),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for output size mismatch");
	sz = sizeof(epoch) + 1;
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&epoch, &sz, NULL, 0),
	    EINVAL,
	    "mallctlbymib() should return EINVAL for output size mismatch");
}
TEST_END

TEST_BEGIN(test_ctl_mib_unsigned) {
	size_t   mib[2];
	unsigned result = 0;

	mib[1] = UINT_MAX;
	expect_d_eq(ctl_mib_unsigned(&result, mib, 1), 0,
	    "Unexpected ctl_mib_unsigned failure");
	expect_u_eq(result, UINT_MAX, "Unexpected unsigned mib value");

	test_skip_if(SIZE_MAX <= UINT_MAX);

	result = 42;
	mib[1] = (size_t)UINT_MAX + 1;
	expect_d_eq(ctl_mib_unsigned(&result, mib, 1), EFAULT,
	    "Expected ctl_mib_unsigned overflow failure");
	expect_u_eq(result, 42, "ctl_mib_unsigned modified output on failure");
}
TEST_END

TEST_BEGIN(test_ctl_verify_read) {
	unsigned old = 0;
	size_t   oldlen = sizeof(old);

	expect_d_eq(ctl_verify_read(&old, &oldlen, sizeof(old)), 0,
	    "Unexpected ctl_verify_read() failure");
	expect_zu_eq(oldlen, sizeof(old), "Unexpected oldlen update");

	oldlen = sizeof(old);
	expect_d_eq(ctl_verify_read(NULL, &oldlen, sizeof(old)), EINVAL,
	    "Unexpected ctl_verify_read() success");
	expect_zu_eq(oldlen, 0, "Unexpected oldlen value");

	expect_d_eq(ctl_verify_read(&old, NULL, sizeof(old)), EINVAL,
	    "Unexpected ctl_verify_read() success");

	oldlen = sizeof(old) - 1;
	expect_d_eq(ctl_verify_read(&old, &oldlen, sizeof(old)), EINVAL,
	    "Unexpected ctl_verify_read() success");
	expect_zu_eq(oldlen, 0, "Unexpected oldlen value");

	oldlen = sizeof(old) + 1;
	expect_d_eq(ctl_verify_read(&old, &oldlen, sizeof(old)), EINVAL,
	    "Unexpected ctl_verify_read() success");
	expect_zu_eq(oldlen, 0, "Unexpected oldlen value");
}
TEST_END

TEST_BEGIN(test_ctl_readonly) {
	unsigned newval = 0;

	/* No write input provided: read-only access is permitted. */
	expect_d_eq(ctl_readonly(NULL, 0), 0,
	    "Unexpected ctl_readonly() failure");

	/* A non-NULL newp is a write attempt: forbidden. */
	expect_d_eq(ctl_readonly(&newval, 0), EPERM,
	    "Unexpected ctl_readonly() success");

	/* A nonzero newlen is a write attempt: forbidden. */
	expect_d_eq(ctl_readonly(NULL, sizeof(newval)), EPERM,
	    "Unexpected ctl_readonly() success");

	/* Both set: forbidden. */
	expect_d_eq(ctl_readonly(&newval, sizeof(newval)), EPERM,
	    "Unexpected ctl_readonly() success");
}
TEST_END

TEST_BEGIN(test_ctl_neither_read_nor_write) {
	unsigned val = 0;
	size_t   len = sizeof(val);

	/* No input or output supplied at all: permitted. */
	expect_d_eq(ctl_neither_read_nor_write(NULL, NULL, NULL, 0), 0,
	    "Unexpected ctl_neither_read_nor_write() failure");

	/* Any output pointer is a read attempt: forbidden. */
	expect_d_eq(ctl_neither_read_nor_write(&val, NULL, NULL, 0), EPERM,
	    "Unexpected success with non-NULL oldp");
	expect_d_eq(ctl_neither_read_nor_write(NULL, &len, NULL, 0), EPERM,
	    "Unexpected success with non-NULL oldlenp");

	/* Any input is a write attempt: forbidden. */
	expect_d_eq(ctl_neither_read_nor_write(NULL, NULL, &val, 0), EPERM,
	    "Unexpected success with non-NULL newp");
	expect_d_eq(ctl_neither_read_nor_write(NULL, NULL, NULL, sizeof(val)),
	    EPERM, "Unexpected success with nonzero newlen");
}
TEST_END

TEST_BEGIN(test_ctl_read_xor_write) {
	unsigned val = 0;
	size_t   len = sizeof(val);

	/* Read only: allowed. */
	expect_d_eq(ctl_read_xor_write(&val, &len, NULL, 0), 0,
	    "Unexpected failure for read-only");

	/* Write only: allowed. */
	expect_d_eq(ctl_read_xor_write(NULL, NULL, &val, sizeof(val)), 0,
	    "Unexpected failure for write-only");

	/* Neither: allowed. */
	expect_d_eq(ctl_read_xor_write(NULL, NULL, NULL, 0), 0,
	    "Unexpected failure for neither");

	/* Both read and write: forbidden. */
	expect_d_eq(ctl_read_xor_write(&val, &len, &val, sizeof(val)), EPERM,
	    "Unexpected success for read+write");

	/* Read plus a nonzero newlen also counts as both: forbidden. */
	expect_d_eq(ctl_read_xor_write(&val, &len, NULL, sizeof(val)), EPERM,
	    "Unexpected success for read + nonzero newlen");

	/*
	 * A half-specified read (oldp set but oldlenp NULL) is not a read, so
	 * pairing it with a write is allowed.
	 */
	expect_d_eq(ctl_read_xor_write(&val, NULL, &val, sizeof(val)), 0,
	    "Unexpected failure when oldlenp is NULL");
}
TEST_END

TEST_BEGIN(test_mallctl_read_write) {
	uint64_t old_epoch, new_epoch;
	size_t   sz = sizeof(old_epoch);

	/* Blind. */
	expect_d_eq(mallctl("epoch", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Read. */
	expect_d_eq(mallctl("epoch", (void *)&old_epoch, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Write. */
	expect_d_eq(
	    mallctl("epoch", NULL, NULL, (void *)&new_epoch, sizeof(new_epoch)),
	    0, "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");

	/* Read+write. */
	expect_d_eq(mallctl("epoch", (void *)&old_epoch, &sz,
	                (void *)&new_epoch, sizeof(new_epoch)),
	    0, "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(old_epoch), "Unexpected output size");
}
TEST_END

TEST_BEGIN(test_mallctl_read_partial) {
	uint64_t epoch;
	size_t   sz = sizeof(epoch);

	expect_d_eq(mallctl("epoch", (void *)&epoch, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(epoch), "Unexpected output size");

	unsigned char misaligned[sizeof(epoch) + 1];
	memset(misaligned, 0, sizeof(misaligned));
	sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", (void *)&misaligned[1], &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure for misaligned output");
	expect_zu_eq(sz, sizeof(epoch), "Unexpected output size");
	expect_d_eq(memcmp(&epoch, &misaligned[1], sizeof(epoch)), 0,
	    "Unexpected value for misaligned output");

	unsigned char short_buf[sizeof(epoch)];
	memset(short_buf, 0, sizeof(short_buf));
	sz = sizeof(epoch) - 1;
	expect_d_eq(mallctl("epoch", (void *)short_buf, &sz, NULL, 0), EINVAL,
	    "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(epoch) - 1, "Unexpected short output size");
	expect_d_eq(memcmp(&epoch, short_buf, sz), 0,
	    "Unexpected short partial output");

	unsigned char long_buf[sizeof(epoch) + 1];
	memset(long_buf, 0xa5, sizeof(long_buf));
	sz = sizeof(epoch) + 1;
	expect_d_eq(mallctl("epoch", (void *)long_buf, &sz, NULL, 0), EINVAL,
	    "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(epoch), "Unexpected long output size");
	expect_d_eq(memcmp(&epoch, long_buf, sizeof(epoch)), 0,
	    "Unexpected long partial output");
	expect_u_eq(long_buf[sizeof(epoch)], 0xa5,
	    "Mallctl wrote past copied output");
}
TEST_END

TEST_BEGIN(test_tcache_create_errors) {
	unsigned tcache_ind = 0;
	size_t   sz = sizeof(tcache_ind);

	/* A non-NULL newp is a write attempt on a read-only ctl: EPERM. */
	expect_d_eq(mallctl("tcache.create", (void *)&tcache_ind, &sz,
	                (void *)&tcache_ind, sizeof(tcache_ind)),
	    EPERM, "tcache.create should reject a write");

	/* A nonzero newlen alone is still a write attempt: EPERM. */
	sz = sizeof(tcache_ind);
	expect_d_eq(mallctl("tcache.create", (void *)&tcache_ind, &sz, NULL,
	                sizeof(tcache_ind)),
	    EPERM, "tcache.create should reject a nonzero newlen");

	/* Missing output buffer: EINVAL with oldlenp zeroed. */
	sz = sizeof(tcache_ind);
	expect_d_eq(mallctl("tcache.create", NULL, &sz, NULL, 0), EINVAL,
	    "tcache.create requires an output buffer");
	expect_zu_eq(sz, 0, "Unexpected oldlen after verify failure");

	/* Undersized output buffer: EINVAL with oldlenp zeroed. */
	sz = sizeof(tcache_ind) - 1;
	expect_d_eq(mallctl("tcache.create", (void *)&tcache_ind, &sz, NULL, 0),
	    EINVAL, "tcache.create should reject an undersized output");
	expect_zu_eq(sz, 0, "Unexpected oldlen after verify failure");

	/* Oversized output buffer: EINVAL with oldlenp zeroed. */
	sz = sizeof(tcache_ind) + 1;
	expect_d_eq(mallctl("tcache.create", (void *)&tcache_ind, &sz, NULL, 0),
	    EINVAL, "tcache.create should reject an oversized output");
	expect_zu_eq(sz, 0, "Unexpected oldlen after verify failure");

	/* Valid read: succeeds, oldlen preserved, index is usable. */
	sz = sizeof(tcache_ind);
	expect_d_eq(mallctl("tcache.create", (void *)&tcache_ind, &sz, NULL, 0),
	    0, "Unexpected tcache.create failure");
	expect_zu_eq(sz, sizeof(tcache_ind), "Unexpected oldlen after success");
	expect_d_eq(mallctl("tcache.destroy", NULL, NULL, (void *)&tcache_ind,
	                sizeof(tcache_ind)),
	    0, "Unexpected tcache.destroy failure");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_short_mib) {
	size_t mib[4];
	size_t miblen;

	miblen = 3;
	mib[3] = 42;
	expect_d_eq(mallctlnametomib("arenas.bin.0.nregs", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	expect_zu_eq(miblen, 3, "Unexpected mib output length");
	expect_zu_eq(mib[3], 42,
	    "mallctlnametomib() wrote past the end of the input mib");
}
TEST_END

TEST_BEGIN(test_mallctlnametomib_short_name) {
	size_t mib[4];
	size_t miblen;

	miblen = 4;
	mib[3] = 42;
	expect_d_eq(mallctlnametomib("arenas.bin.0", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	expect_zu_eq(miblen, 3, "Unexpected mib output length");
	expect_zu_eq(mib[3], 42,
	    "mallctlnametomib() wrote past the end of the input mib");
}
TEST_END

TEST_BEGIN(test_mallctlmibnametomib) {
	size_t   mib[4];
	size_t   miblen = 4;
	uint32_t result, result_ref;
	size_t   len_result = sizeof(uint32_t);

	tsd_t *tsd = tsd_fetch();

	/* Error cases */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 0, "bob", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(ctl_mibnametomib(tsd, mib, 0, "9999", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid case. */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 0, "arenas", &miblen), 0, "");
	assert_zu_eq(miblen, 1, "");
	miblen = 4;
	assert_d_eq(ctl_mibnametomib(tsd, mib, 1, "bin", &miblen), 0, "");
	assert_zu_eq(miblen, 2, "");
	expect_d_eq(mallctlbymib(mib, miblen, &result, &len_result, NULL, 0),
	    ENOENT, "mallctlbymib() should fail on partial path");

	/* Error cases. */
	miblen = 4;
	assert_d_eq(ctl_mibnametomib(tsd, mib, 2, "bob", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(ctl_mibnametomib(tsd, mib, 2, "9999", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid case. */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 2, "0", &miblen), 0, "");
	assert_zu_eq(miblen, 3, "");
	expect_d_eq(mallctlbymib(mib, miblen, &result, &len_result, NULL, 0),
	    ENOENT, "mallctlbymib() should fail on partial path");

	/* Error cases. */
	miblen = 4;
	assert_d_eq(ctl_mibnametomib(tsd, mib, 3, "bob", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(ctl_mibnametomib(tsd, mib, 3, "9999", &miblen), ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid case. */
	assert_d_eq(ctl_mibnametomib(tsd, mib, 3, "nregs", &miblen), 0, "");
	assert_zu_eq(miblen, 4, "");
	assert_d_eq(mallctlbymib(mib, miblen, &result, &len_result, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
	assert_d_eq(
	    mallctl("arenas.bin.0.nregs", &result_ref, &len_result, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(result, result_ref,
	    "mallctlbymib() and mallctl() returned different result");
}
TEST_END

TEST_BEGIN(test_mallctlbymibname) {
	size_t   mib[4];
	size_t   miblen = 4;
	uint32_t result, result_ref;
	size_t   len_result = sizeof(uint32_t);

	tsd_t *tsd = tsd_fetch();

	/* Error cases. */

	assert_d_eq(mallctlnametomib("arenas", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	assert_zu_eq(miblen, 1, "");

	miblen = 4;
	assert_d_eq(ctl_bymibname(tsd, mib, 1, "bin.0", &miblen, &result,
	                &len_result, NULL, 0),
	    ENOENT, "");
	miblen = 4;
	assert_d_eq(ctl_bymibname(tsd, mib, 1, "bin.0.bob", &miblen, &result,
	                &len_result, NULL, 0),
	    ENOENT, "");
	assert_zu_eq(miblen, 4, "");

	/* Valid cases. */

	assert_d_eq(
	    mallctl("arenas.bin.0.nregs", &result_ref, &len_result, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	miblen = 4;

	assert_d_eq(ctl_bymibname(tsd, mib, 0, "arenas.bin.0.nregs", &miblen,
	                &result, &len_result, NULL, 0),
	    0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");

	assert_d_eq(ctl_bymibname(tsd, mib, 1, "bin.0.nregs", &miblen, &result,
	                &len_result, NULL, 0),
	    0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");

	assert_d_eq(ctl_bymibname(tsd, mib, 2, "0.nregs", &miblen, &result,
	                &len_result, NULL, 0),
	    0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");

	assert_d_eq(ctl_bymibname(tsd, mib, 3, "nregs", &miblen, &result,
	                &len_result, NULL, 0),
	    0, "");
	assert_zu_eq(miblen, 4, "");
	expect_zu_eq(result, result_ref, "Unexpected result");
}
TEST_END

TEST_BEGIN(test_mallctl_config) {
#define TEST_MALLCTL_CONFIG(config, t)                                         \
	do {                                                                   \
		t      oldval;                                                 \
		size_t sz = sizeof(oldval);                                    \
		expect_d_eq(                                                   \
		    mallctl("config." #config, (void *)&oldval, &sz, NULL, 0), \
		    0, "Unexpected mallctl() failure");                        \
		expect_b_eq(                                                   \
		    oldval, config_##config, "Incorrect config value");        \
		expect_zu_eq(sz, sizeof(oldval), "Unexpected output size");    \
	} while (0)

	TEST_MALLCTL_CONFIG(cache_oblivious, bool);
	TEST_MALLCTL_CONFIG(debug, bool);
	TEST_MALLCTL_CONFIG(fill, bool);
	TEST_MALLCTL_CONFIG(infallible_new, bool);
	TEST_MALLCTL_CONFIG(lazy_lock, bool);
	TEST_MALLCTL_CONFIG(malloc_conf, const char *);
	TEST_MALLCTL_CONFIG(prof, bool);
	TEST_MALLCTL_CONFIG(prof_libgcc, bool);
	TEST_MALLCTL_CONFIG(prof_libunwind, bool);
	TEST_MALLCTL_CONFIG(prof_frameptr, bool);
	TEST_MALLCTL_CONFIG(stats, bool);
	TEST_MALLCTL_CONFIG(utrace, bool);
	TEST_MALLCTL_CONFIG(xmalloc, bool);

#undef TEST_MALLCTL_CONFIG
}
TEST_END

TEST_BEGIN(test_mallctl_opt) {
	bool config_always = true;

#define TEST_MALLCTL_OPT(t, opt, config)                                       \
	do {                                                                   \
		t      oldval;                                                 \
		size_t sz = sizeof(oldval);                                    \
		int    expected = config_##config ? 0 : ENOENT;                \
		int    result = mallctl(                                       \
                    "opt." #opt, (void *)&oldval, &sz, NULL, 0);            \
		expect_d_eq(result, expected,                                  \
		    "Unexpected mallctl() result for opt." #opt);              \
		expect_zu_eq(sz, sizeof(oldval), "Unexpected output size");    \
	} while (0)

	TEST_MALLCTL_OPT(bool, abort, always);
	TEST_MALLCTL_OPT(bool, abort_conf, always);
	TEST_MALLCTL_OPT(bool, cache_oblivious, always);
	TEST_MALLCTL_OPT(bool, trust_madvise, always);
	TEST_MALLCTL_OPT(
	    bool, experimental_hpa_start_huge_if_thp_always, always);
	TEST_MALLCTL_OPT(bool, experimental_hpa_enforce_hugify, always);
	TEST_MALLCTL_OPT(bool, confirm_conf, always);
	TEST_MALLCTL_OPT(const char *, metadata_thp, always);
	TEST_MALLCTL_OPT(bool, retain, always);
	TEST_MALLCTL_OPT(const char *, dss, always);
	TEST_MALLCTL_OPT(bool, hpa, always);
	TEST_MALLCTL_OPT(size_t, hpa_slab_max_alloc, always);
	TEST_MALLCTL_OPT(bool, hpa_hugify_sync, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_nshards, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_max_alloc, always);
	TEST_MALLCTL_OPT(size_t, hpa_sec_max_bytes, always);
	TEST_MALLCTL_OPT(size_t, experimental_pac_sec_nshards, always);
	TEST_MALLCTL_OPT(size_t, experimental_pac_sec_max_alloc, always);
	TEST_MALLCTL_OPT(size_t, experimental_pac_sec_max_bytes, always);
	TEST_MALLCTL_OPT(size_t, hpa_purge_threshold, always);
	TEST_MALLCTL_OPT(uint64_t, hpa_min_purge_delay_ms, always);
	TEST_MALLCTL_OPT(const char *, hpa_hugify_style, always);
	TEST_MALLCTL_OPT(unsigned, narenas, always);
	TEST_MALLCTL_OPT(const char *, percpu_arena, always);
	TEST_MALLCTL_OPT(size_t, oversize_threshold, always);
	TEST_MALLCTL_OPT(bool, background_thread, always);
	TEST_MALLCTL_OPT(ssize_t, dirty_decay_ms, always);
	TEST_MALLCTL_OPT(ssize_t, muzzy_decay_ms, always);
	TEST_MALLCTL_OPT(bool, stats_print, always);
	TEST_MALLCTL_OPT(const char *, stats_print_opts, always);
	TEST_MALLCTL_OPT(int64_t, stats_interval, always);
	TEST_MALLCTL_OPT(const char *, stats_interval_opts, always);
	TEST_MALLCTL_OPT(const char *, junk, fill);
	TEST_MALLCTL_OPT(bool, zero, fill);
	TEST_MALLCTL_OPT(bool, utrace, utrace);
	TEST_MALLCTL_OPT(bool, xmalloc, xmalloc);
	TEST_MALLCTL_OPT(bool, tcache, always);
	TEST_MALLCTL_OPT(size_t, lg_extent_max_active_fit, always);
	TEST_MALLCTL_OPT(size_t, tcache_max, always);
	TEST_MALLCTL_OPT(const char *, thp, always);
	TEST_MALLCTL_OPT(const char *, zero_realloc, always);
	TEST_MALLCTL_OPT(bool, prof, prof);
	TEST_MALLCTL_OPT(const char *, prof_prefix, prof);
	TEST_MALLCTL_OPT(bool, prof_active, prof);
	TEST_MALLCTL_OPT(unsigned, prof_bt_max, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_prof_sample, prof);
	TEST_MALLCTL_OPT(bool, prof_accum, prof);
	TEST_MALLCTL_OPT(bool, prof_pid_namespace, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_prof_interval, prof);
	TEST_MALLCTL_OPT(bool, prof_gdump, prof);
	TEST_MALLCTL_OPT(bool, prof_final, prof);
	TEST_MALLCTL_OPT(bool, prof_leak, prof);
	TEST_MALLCTL_OPT(bool, prof_leak_error, prof);
	TEST_MALLCTL_OPT(ssize_t, prof_recent_alloc_max, prof);
	TEST_MALLCTL_OPT(bool, prof_stats, prof);
	TEST_MALLCTL_OPT(bool, prof_sys_thread_name, prof);
	TEST_MALLCTL_OPT(ssize_t, lg_san_uaf_align, uaf_detection);
	TEST_MALLCTL_OPT(unsigned, debug_double_free_max_scan, always);
	TEST_MALLCTL_OPT(bool, disable_large_size_classes, always);
	TEST_MALLCTL_OPT(size_t, process_madvise_max_batch, always);

#undef TEST_MALLCTL_OPT
}
TEST_END

TEST_BEGIN(test_manpage_example) {
	unsigned nbins, i;
	size_t   mib[4];
	size_t   len, miblen;

	len = sizeof(nbins);
	expect_d_eq(mallctl("arenas.nbins", (void *)&nbins, &len, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	miblen = 4;
	expect_d_eq(mallctlnametomib("arenas.bin.0.size", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	for (i = 0; i < nbins; i++) {
		size_t bin_size;

		mib[2] = i;
		len = sizeof(bin_size);
		expect_d_eq(
		    mallctlbymib(mib, miblen, (void *)&bin_size, &len, NULL, 0),
		    0, "Unexpected mallctlbymib() failure");
		/* Do something with bin_size... */
	}
}
TEST_END

TEST_BEGIN(test_tcache_none) {
	test_skip_if(!opt_tcache);

	/* Allocate p and q. */
	void *p0 = mallocx(42, 0);
	expect_ptr_not_null(p0, "Unexpected mallocx() failure");
	void *q = mallocx(42, 0);
	expect_ptr_not_null(q, "Unexpected mallocx() failure");

	/* Deallocate p and q, but bypass the tcache for q. */
	dallocx(p0, 0);
	dallocx(q, MALLOCX_TCACHE_NONE);

	/* Make sure that tcache-based allocation returns p, not q. */
	void *p1 = mallocx(42, 0);
	expect_ptr_not_null(p1, "Unexpected mallocx() failure");
	if (!opt_prof && !san_uaf_detection_enabled()) {
		expect_ptr_eq(
		    p0, p1, "Expected tcache to allocate cached region");
	}

	/* Clean up. */
	dallocx(p1, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_tcache) {
#define NTCACHES 10
	unsigned tis[NTCACHES];
	void    *ps[NTCACHES];
	void    *qs[NTCACHES];
	unsigned i;
	size_t   sz, psz, qsz;

	psz = 42;
	qsz = nallocx(psz, 0) + 1;

	/* Create tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		sz = sizeof(unsigned);
		expect_d_eq(
		    mallctl("tcache.create", (void *)&tis[i], &sz, NULL, 0), 0,
		    "Unexpected mallctl() failure, i=%u", i);
	}

	/* Exercise tcache ID recycling. */
	for (i = 0; i < NTCACHES; i++) {
		expect_d_eq(mallctl("tcache.destroy", NULL, NULL,
		                (void *)&tis[i], sizeof(unsigned)),
		    0, "Unexpected mallctl() failure, i=%u", i);
	}
	for (i = 0; i < NTCACHES; i++) {
		sz = sizeof(unsigned);
		expect_d_eq(
		    mallctl("tcache.create", (void *)&tis[i], &sz, NULL, 0), 0,
		    "Unexpected mallctl() failure, i=%u", i);
	}

	/* Flush empty tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		expect_d_eq(mallctl("tcache.flush", NULL, NULL, (void *)&tis[i],
		                sizeof(unsigned)),
		    0, "Unexpected mallctl() failure, i=%u", i);
	}

	/* Cache some allocations. */
	for (i = 0; i < NTCACHES; i++) {
		ps[i] = mallocx(psz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(
		    ps[i], "Unexpected mallocx() failure, i=%u", i);
		dallocx(ps[i], MALLOCX_TCACHE(tis[i]));

		qs[i] = mallocx(qsz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(
		    qs[i], "Unexpected mallocx() failure, i=%u", i);
		dallocx(qs[i], MALLOCX_TCACHE(tis[i]));
	}

	/* Verify that tcaches allocate cached regions. */
	for (i = 0; i < NTCACHES; i++) {
		void *p0 = ps[i];
		ps[i] = mallocx(psz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(
		    ps[i], "Unexpected mallocx() failure, i=%u", i);
		if (!san_uaf_detection_enabled()) {
			expect_ptr_eq(ps[i], p0,
			    "Expected mallocx() to "
			    "allocate cached region, i=%u",
			    i);
		}
	}

	/* Verify that reallocation uses cached regions. */
	for (i = 0; i < NTCACHES; i++) {
		void *q0 = qs[i];
		qs[i] = rallocx(ps[i], qsz, MALLOCX_TCACHE(tis[i]));
		expect_ptr_not_null(
		    qs[i], "Unexpected rallocx() failure, i=%u", i);
		if (!san_uaf_detection_enabled()) {
			expect_ptr_eq(qs[i], q0,
			    "Expected rallocx() to "
			    "allocate cached region, i=%u",
			    i);
		}
		/* Avoid undefined behavior in case of test failure. */
		if (qs[i] == NULL) {
			qs[i] = ps[i];
		}
	}
	for (i = 0; i < NTCACHES; i++) {
		dallocx(qs[i], MALLOCX_TCACHE(tis[i]));
	}

	/* Flush some non-empty tcaches. */
	for (i = 0; i < NTCACHES / 2; i++) {
		expect_d_eq(mallctl("tcache.flush", NULL, NULL, (void *)&tis[i],
		                sizeof(unsigned)),
		    0, "Unexpected mallctl() failure, i=%u", i);
	}

	/* Destroy tcaches. */
	for (i = 0; i < NTCACHES; i++) {
		expect_d_eq(mallctl("tcache.destroy", NULL, NULL,
		                (void *)&tis[i], sizeof(unsigned)),
		    0, "Unexpected mallctl() failure, i=%u", i);
	}
}
TEST_END

TEST_BEGIN(test_thread_arena) {
	unsigned old_arena_ind, new_arena_ind, narenas;

	const char *opa;
	size_t      sz = sizeof(opa);
	expect_d_eq(mallctl("opt.percpu_arena", (void *)&opa, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	if (opt_oversize_threshold != 0) {
		narenas--;
	}
	expect_u_eq(narenas, opt_narenas, "Number of arenas incorrect");

	if (strcmp(opa, "disabled") == 0) {
		new_arena_ind = narenas - 1;
		expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		                (void *)&new_arena_ind, sizeof(unsigned)),
		    0, "Unexpected mallctl() failure");
		new_arena_ind = 0;
		expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		                (void *)&new_arena_ind, sizeof(unsigned)),
		    0, "Unexpected mallctl() failure");
	} else {
		expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
		                NULL, 0),
		    0, "Unexpected mallctl() failure");
		new_arena_ind = percpu_arena_ind_limit(opt_percpu_arena) - 1;
		if (old_arena_ind != new_arena_ind) {
			expect_d_eq(
			    mallctl("thread.arena", (void *)&old_arena_ind, &sz,
			        (void *)&new_arena_ind, sizeof(unsigned)),
			    EPERM,
			    "thread.arena ctl "
			    "should not be allowed with percpu arena");
		}
	}
}
TEST_END

TEST_BEGIN(test_thread_arena_bad_oldlen_no_migrate) {
	unsigned original_arena_ind, new_arena_ind, after_arena_ind;
	size_t   sz = sizeof(unsigned);

	expect_d_eq(mallctl("thread.arena", (void *)&original_arena_ind, &sz,
	                NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_zu_eq(sz, sizeof(original_arena_ind), "Unexpected output size");

	sz = sizeof(new_arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&new_arena_ind, &sz, NULL,
	                0),
	    0, "Unexpected arenas.create failure");
	expect_zu_eq(sz, sizeof(new_arena_ind), "Unexpected output size");
	expect_u_ne(new_arena_ind, original_arena_ind,
	    "New arena unexpectedly matches current thread arena");

	unsigned char short_buf[sizeof(original_arena_ind)];
	memset(short_buf, 0, sizeof(short_buf));
	sz = sizeof(original_arena_ind) - 1;
	expect_d_eq(mallctl("thread.arena", (void *)short_buf, &sz,
	                (void *)&new_arena_ind, sizeof(new_arena_ind)),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(original_arena_ind) - 1,
	    "Unexpected short output size");
	expect_d_eq(memcmp(&original_arena_ind, short_buf, sz), 0,
	    "Unexpected short partial output");

	sz = sizeof(after_arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&after_arena_ind, &sz, NULL,
	                0),
	    0, "Unexpected mallctl() failure");
	expect_u_eq(after_arena_ind, original_arena_ind,
	    "Thread migrated despite short output buffer");

	unsigned char long_buf[sizeof(original_arena_ind) + 1];
	memset(long_buf, 0xa5, sizeof(long_buf));
	sz = sizeof(original_arena_ind) + 1;
	expect_d_eq(mallctl("thread.arena", (void *)long_buf, &sz,
	                (void *)&new_arena_ind, sizeof(new_arena_ind)),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(original_arena_ind),
	    "Unexpected long output size");
	expect_d_eq(memcmp(&original_arena_ind, long_buf,
	                sizeof(original_arena_ind)),
	    0, "Unexpected long partial output");
	expect_u_eq(long_buf[sizeof(original_arena_ind)], 0xa5,
	    "Mallctl wrote past copied output");

	sz = sizeof(after_arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&after_arena_ind, &sz, NULL,
	                0),
	    0, "Unexpected mallctl() failure");
	expect_u_eq(after_arena_ind, original_arena_ind,
	    "Thread migrated despite long output buffer");
}
TEST_END

TEST_BEGIN(test_arena_i_initialized) {
	unsigned narenas, i;
	size_t   sz;
	size_t   mib[3];
	size_t   miblen = sizeof(mib) / sizeof(size_t);
	bool     initialized;

	sz = sizeof(narenas);
	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	expect_d_eq(mallctlnametomib("arena.0.initialized", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	for (i = 0; i < narenas; i++) {
		mib[1] = i;
		sz = sizeof(initialized);
		expect_d_eq(
		    mallctlbymib(mib, miblen, &initialized, &sz, NULL, 0), 0,
		    "Unexpected mallctl() failure");
	}

	mib[1] = MALLCTL_ARENAS_ALL;
	sz = sizeof(initialized);
	expect_d_eq(mallctlbymib(mib, miblen, &initialized, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_true(initialized,
	    "Merged arena statistics should always be initialized");

	/* Equivalent to the above but using mallctl() directly. */
	sz = sizeof(initialized);
	expect_d_eq(
	    mallctl("arena." STRINGIFY(MALLCTL_ARENAS_ALL) ".initialized",
	        (void *)&initialized, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_true(initialized,
	    "Merged arena statistics should always be initialized");
}
TEST_END

TEST_BEGIN(test_arena_i_initialized_errors) {
	bool          initialized = false;
	unsigned char buf[sizeof(bool) + 1];
	size_t        sz;

	/* Write attempt on a read-only ctl: EPERM. */
	sz = sizeof(initialized);
	expect_d_eq(mallctl("arena.0.initialized", (void *)&initialized, &sz,
	                (void *)&initialized, sizeof(initialized)),
	    EPERM, "arena.i.initialized should reject a write");

	/* Undersized output buffer: EINVAL with oldlenp truncated. */
	sz = 0;
	expect_d_eq(mallctl("arena.0.initialized", (void *)buf, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, 0, "Unexpected truncated oldlen");

	/* Oversized output buffer: EINVAL, only sizeof(bool) reported/copied. */
	memset(buf, 0xa5, sizeof(buf));
	sz = sizeof(bool) + 1;
	expect_d_eq(mallctl("arena.0.initialized", (void *)buf, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(bool), "Unexpected truncated oldlen");
	expect_u_eq(buf[sizeof(bool)], 0xa5,
	    "Mallctl wrote past copied output");
}
TEST_END

TEST_BEGIN(test_thread_tcache_enabled_errors) {
	bool   enabled, prev;
	size_t sz = sizeof(bool);

	/* Capture current state so we can restore it at the end. */
	expect_d_eq(
	    mallctl("thread.tcache.enabled", (void *)&prev, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	/* (1) Write with wrong newlen: EINVAL, and the state is unchanged. */
	bool newval = !prev;
	expect_d_eq(mallctl("thread.tcache.enabled", NULL, NULL,
	                (void *)&newval, sizeof(bool) + 1),
	    EINVAL, "Expected input size mismatch");
	sz = sizeof(bool);
	expect_d_eq(
	    mallctl("thread.tcache.enabled", (void *)&enabled, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_b_eq(enabled, prev,
	    "Enabled state should be unchanged after a failed write");

	/* (2) Pure read, undersized oldlen: EINVAL with oldlenp truncated. */
	unsigned char buf[sizeof(bool) + 1];
	memset(buf, 0xa5, sizeof(buf));
	sz = 0;
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)buf, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, 0, "Unexpected truncated oldlen");

	/* (2) Pure read, oversized oldlen: EINVAL, copies sizeof(bool) only. */
	sz = sizeof(bool) + 1;
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)buf, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(bool), "Unexpected truncated oldlen");
	expect_u_eq(buf[sizeof(bool)], 0xa5, "Mallctl wrote past copied output");

	/* (3) Valid newlen but bad oldlen: EINVAL, yet the write still applies. */
	sz = sizeof(bool) + 1;
	newval = !prev;
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)buf, &sz,
	                (void *)&newval, sizeof(bool)),
	    EINVAL, "Expected output size mismatch");
	sz = sizeof(bool);
	expect_d_eq(
	    mallctl("thread.tcache.enabled", (void *)&enabled, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_b_eq(enabled, newval,
	    "A valid write should take effect even when read-out fails");

	/* Restore the original state. */
	expect_d_eq(mallctl("thread.tcache.enabled", NULL, NULL, (void *)&prev,
	                sizeof(bool)),
	    0, "Unexpected mallctl() failure");
}
TEST_END

TEST_BEGIN(test_thread_peak_read_errors) {
	test_skip_if(!config_stats);

	uint64_t peak = 0;
	size_t   sz = sizeof(peak);

	/* Write attempt on a read-only ctl: EPERM. */
	expect_d_eq(mallctl("thread.peak.read", (void *)&peak, &sz,
	                (void *)&peak, sizeof(peak)),
	    EPERM, "thread.peak.read should reject a write");

	/* Undersized output buffer: EINVAL, copies sizeof - 1, no overrun. */
	unsigned char buf[sizeof(uint64_t) + 1];
	memset(buf, 0xa5, sizeof(buf));
	sz = sizeof(uint64_t) - 1;
	expect_d_eq(mallctl("thread.peak.read", (void *)buf, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(uint64_t) - 1, "Unexpected truncated oldlen");
	expect_u_eq(buf[sizeof(uint64_t) - 1], 0xa5,
	    "Mallctl wrote past requested output");

	/* Oversized output buffer: EINVAL, copies sizeof only, no overrun. */
	memset(buf, 0xa5, sizeof(buf));
	sz = sizeof(uint64_t) + 1;
	expect_d_eq(mallctl("thread.peak.read", (void *)buf, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(uint64_t), "Unexpected truncated oldlen");
	expect_u_eq(buf[sizeof(uint64_t)], 0xa5,
	    "Mallctl wrote past copied output");
}
TEST_END

/*
 * Shared error-path checks for a scalar read+write ctl: a wrong newlen is
 * rejected with EINVAL without changing the value, and a wrong oldlen on read
 * truncates oldlenp to the number of bytes copied.
 */
static void
expect_scalar_rw_errors(const char *name, size_t valsz) {
	unsigned char buf[16];
	unsigned char saved[16];
	size_t        sz;

	sz = valsz;
	expect_d_eq(mallctl(name, (void *)saved, &sz, NULL, 0), 0,
	    "Unexpected read failure for %s", name);
	expect_zu_eq(sz, valsz, "Unexpected output size for %s", name);

	/* Wrong newlen: EINVAL, value unchanged. */
	expect_d_eq(mallctl(name, NULL, NULL, (void *)saved, valsz + 1), EINVAL,
	    "Expected input size mismatch for %s", name);
	memset(buf, 0, sizeof(buf));
	sz = valsz;
	expect_d_eq(mallctl(name, (void *)buf, &sz, NULL, 0), 0,
	    "Unexpected read failure for %s", name);
	expect_d_eq(memcmp(buf, saved, valsz), 0,
	    "%s changed after a failed write", name);

	/* Wrong oldlen on read: EINVAL, oldlenp truncated to the copied len. */
	sz = valsz + 1;
	expect_d_eq(mallctl(name, (void *)buf, &sz, NULL, 0), EINVAL,
	    "Expected output size mismatch for %s", name);
	expect_zu_eq(sz, valsz, "Unexpected truncated oldlen for %s", name);
}

TEST_BEGIN(test_arenas_narenas_errors) {
	unsigned narenas;
	size_t   sz = sizeof(narenas);

	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz,
	                (void *)&narenas, sizeof(narenas)),
	    EPERM, "arenas.narenas should reject a write");

	sz = sizeof(narenas) + 1;
	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(narenas), "Unexpected truncated oldlen");
}
TEST_END

TEST_BEGIN(test_approximate_stats_active_errors) {
	size_t active;
	size_t sz = sizeof(active);

	expect_d_eq(mallctl("approximate_stats.active", (void *)&active, &sz,
	                (void *)&active, sizeof(active)),
	    EPERM, "approximate_stats.active should reject a write");

	sz = sizeof(active) + 1;
	expect_d_eq(
	    mallctl("approximate_stats.active", (void *)&active, &sz, NULL, 0),
	    EINVAL, "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(active), "Unexpected truncated oldlen");
}
TEST_END

TEST_BEGIN(test_decay_ms_oversize_errors) {
	expect_scalar_rw_errors("arena.0.dirty_decay_ms", sizeof(ssize_t));
	expect_scalar_rw_errors("arena.0.muzzy_decay_ms", sizeof(ssize_t));
	expect_scalar_rw_errors("arenas.dirty_decay_ms", sizeof(ssize_t));
	expect_scalar_rw_errors("arenas.muzzy_decay_ms", sizeof(ssize_t));
	expect_scalar_rw_errors("arena.0.oversize_threshold", sizeof(size_t));
}
TEST_END

TEST_BEGIN(test_background_thread_errors) {
	bool   enabled;
	size_t sz = sizeof(enabled);

	/* Skip when background threads are unavailable in this build. */
	test_skip_if(
	    mallctl("background_thread", (void *)&enabled, &sz, NULL, 0) != 0);

	/* Wrong newlen: EINVAL, enabled state unchanged. */
	bool toggled = !enabled;
	expect_d_eq(mallctl("background_thread", NULL, NULL, (void *)&toggled,
	                sizeof(bool) + 1),
	    EINVAL, "Expected input size mismatch");
	bool again;
	sz = sizeof(again);
	expect_d_eq(mallctl("background_thread", (void *)&again, &sz, NULL, 0),
	    0, "Unexpected read failure");
	expect_b_eq(again, enabled,
	    "background_thread changed after a failed write");

	/* max_background_threads: wrong newlen -> EINVAL. */
	size_t maxbt;
	sz = sizeof(maxbt);
	expect_d_eq(
	    mallctl("max_background_threads", (void *)&maxbt, &sz, NULL, 0), 0,
	    "Unexpected read failure");
	expect_d_eq(mallctl("max_background_threads", NULL, NULL, (void *)&maxbt,
	                sizeof(size_t) + 1),
	    EINVAL, "Expected input size mismatch");
}
TEST_END

TEST_BEGIN(test_prof_toggle_errors) {
	bool   prof_enabled;
	size_t sz = sizeof(prof_enabled);
	test_skip_if(
	    mallctl("opt.prof", (void *)&prof_enabled, &sz, NULL, 0) != 0
	    || !prof_enabled);

	const char *names[] = {"prof.active", "prof.gdump",
	    "prof.thread_active_init", "thread.prof.active"};
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		bool   cur;
		size_t s = sizeof(cur);
		expect_d_eq(mallctl(names[i], (void *)&cur, &s, NULL, 0), 0,
		    "Unexpected read failure for %s", names[i]);
		expect_d_eq(mallctl(names[i], NULL, NULL, (void *)&cur,
		                sizeof(bool) + 1),
		    EINVAL, "Expected input size mismatch for %s", names[i]);
		bool after;
		s = sizeof(after);
		expect_d_eq(mallctl(names[i], (void *)&after, &s, NULL, 0), 0,
		    "Unexpected read failure for %s", names[i]);
		expect_b_eq(after, cur, "%s changed after a failed write",
		    names[i]);
	}
}
TEST_END

TEST_BEGIN(test_experimental_prof_recent_alloc_max_errors) {
	bool   prof_enabled;
	size_t sz = sizeof(prof_enabled);
	test_skip_if(
	    mallctl("opt.prof", (void *)&prof_enabled, &sz, NULL, 0) != 0
	    || !prof_enabled);

	const char *name = "experimental.prof_recent.alloc_max";
	ssize_t     orig;
	sz = sizeof(orig);
	expect_d_eq(mallctl(name, (void *)&orig, &sz, NULL, 0), 0,
	    "Unexpected read failure");

	/* Wrong newlen: EINVAL. */
	expect_d_eq(
	    mallctl(name, NULL, NULL, (void *)&orig, sizeof(ssize_t) + 1),
	    EINVAL, "Expected input size mismatch");

	/* Out-of-range value (< -1): EINVAL. */
	ssize_t bad = -2;
	expect_d_eq(mallctl(name, NULL, NULL, (void *)&bad, sizeof(bad)), EINVAL,
	    "Expected EINVAL for max < -1");

	/* Wrong oldlen on read: EINVAL with oldlenp truncated. */
	ssize_t scratch;
	sz = sizeof(ssize_t) + 1;
	expect_d_eq(mallctl(name, (void *)&scratch, &sz, NULL, 0), EINVAL,
	    "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(ssize_t), "Unexpected truncated oldlen");

	/* The failed writes left the value unchanged. */
	ssize_t after;
	sz = sizeof(after);
	expect_d_eq(mallctl(name, (void *)&after, &sz, NULL, 0), 0,
	    "Unexpected read failure");
	expect_zd_eq(after, orig, "alloc_max changed after failed writes");
}
TEST_END

TEST_BEGIN(test_prof_stats_errors) {
	bool   b;
	size_t sz = sizeof(b);
	test_skip_if(mallctl("opt.prof", (void *)&b, &sz, NULL, 0) != 0 || !b);
	sz = sizeof(b);
	test_skip_if(
	    mallctl("opt.prof_stats", (void *)&b, &sz, NULL, 0) != 0 || !b);

	const char *names[] = {"prof.stats.bins.0.live",
	    "prof.stats.bins.0.accum", "prof.stats.lextents.0.live",
	    "prof.stats.lextents.0.accum"};
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		unsigned char buf[8];
		size_t        s = sizeof(buf);

		/* Read-only: a write is rejected before any size check. */
		expect_d_eq(mallctl(names[i], (void *)buf, &s, (void *)buf,
		                sizeof(buf)),
		    EPERM, "%s should reject a write", names[i]);

		/* Size mismatch on read: EINVAL, oldlenp = copied length. */
		s = 1;
		expect_d_eq(mallctl(names[i], (void *)buf, &s, NULL, 0), EINVAL,
		    "Expected output size mismatch for %s", names[i]);
		expect_zu_eq(s, 1, "Unexpected truncated oldlen for %s",
		    names[i]);
	}
}
TEST_END

TEST_BEGIN(test_thread_tcache_flush_errors) {
	/*
	 * A successful flush both confirms tcache is available (so the EPERM
	 * path below is reached rather than EFAULT) and covers the happy path.
	 */
	test_skip_if(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0) != 0);

	unsigned val = 0;
	size_t   sz = sizeof(val);

	/* Command ctl: a read attempt is rejected with EPERM. */
	expect_d_eq(mallctl("thread.tcache.flush", (void *)&val, &sz, NULL, 0),
	    EPERM, "thread.tcache.flush should reject a read");

	/* A write attempt is rejected with EPERM. */
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, (void *)&val,
	                sizeof(val)),
	    EPERM, "thread.tcache.flush should reject a write");
}
TEST_END

TEST_BEGIN(test_thread_peak_reset_errors) {
	test_skip_if(!config_stats);

	unsigned val = 0;
	size_t   sz = sizeof(val);

	/* Command ctl: a read attempt is rejected with EPERM. */
	expect_d_eq(mallctl("thread.peak.reset", (void *)&val, &sz, NULL, 0),
	    EPERM, "thread.peak.reset should reject a read");

	/* A write attempt is rejected with EPERM. */
	expect_d_eq(mallctl("thread.peak.reset", NULL, NULL, (void *)&val,
	                sizeof(val)),
	    EPERM, "thread.peak.reset should reject a write");
}
TEST_END

TEST_BEGIN(test_thread_idle_errors) {
	unsigned val = 0;
	size_t   sz = sizeof(val);

	/* Command ctl: a read attempt is rejected with EPERM. */
	expect_d_eq(mallctl("thread.idle", (void *)&val, &sz, NULL, 0), EPERM,
	    "thread.idle should reject a read");

	/* A write attempt is rejected with EPERM. */
	expect_d_eq(
	    mallctl("thread.idle", NULL, NULL, (void *)&val, sizeof(val)),
	    EPERM, "thread.idle should reject a write");
}
TEST_END

TEST_BEGIN(test_arena_i_decay_errors) {
	unsigned val = 0;
	size_t   sz = sizeof(val);

	/* Command ctl: a read attempt is rejected with EPERM. */
	expect_d_eq(mallctl("arena.0.decay", (void *)&val, &sz, NULL, 0), EPERM,
	    "arena.i.decay should reject a read");

	/* A write attempt is rejected with EPERM. */
	expect_d_eq(
	    mallctl("arena.0.decay", NULL, NULL, (void *)&val, sizeof(val)),
	    EPERM, "arena.i.decay should reject a write");
}
TEST_END

TEST_BEGIN(test_arena_i_purge_errors) {
	unsigned val = 0;
	size_t   sz = sizeof(val);

	/* Command ctl: a read attempt is rejected with EPERM. */
	expect_d_eq(mallctl("arena.0.purge", (void *)&val, &sz, NULL, 0), EPERM,
	    "arena.i.purge should reject a read");

	/* A write attempt is rejected with EPERM. */
	expect_d_eq(
	    mallctl("arena.0.purge", NULL, NULL, (void *)&val, sizeof(val)),
	    EPERM, "arena.i.purge should reject a write");
}
TEST_END

TEST_BEGIN(test_arena_i_reset_destroy_errors) {
	unsigned val = 0;
	size_t   sz = sizeof(val);

	/*
	 * reset/destroy are command ctls sharing one helper that rejects any
	 * I/O with EPERM before touching the arena, so this does not actually
	 * reset or destroy arena 0.
	 */
	expect_d_eq(mallctl("arena.0.reset", (void *)&val, &sz, NULL, 0), EPERM,
	    "arena.i.reset should reject a read");
	expect_d_eq(
	    mallctl("arena.0.reset", NULL, NULL, (void *)&val, sizeof(val)),
	    EPERM, "arena.i.reset should reject a write");
	expect_d_eq(mallctl("arena.0.destroy", (void *)&val, &sz, NULL, 0),
	    EPERM, "arena.i.destroy should reject a read");
	expect_d_eq(
	    mallctl("arena.0.destroy", NULL, NULL, (void *)&val, sizeof(val)),
	    EPERM, "arena.i.destroy should reject a write");
}
TEST_END

TEST_BEGIN(test_thread_prof_name_errors) {
	bool   prof_enabled;
	size_t sz = sizeof(prof_enabled);
	test_skip_if(
	    mallctl("opt.prof", (void *)&prof_enabled, &sz, NULL, 0) != 0
	    || !prof_enabled);

	/*
	 * thread.prof.name takes a (const char *); a wrong newlen is rejected
	 * with EINVAL.  (EPERM on read+write, NULL input, and the read path are
	 * covered by test/unit/prof_thread_name.c.)
	 */
	const char *name = "x";
	expect_d_eq(mallctl("thread.prof.name", NULL, NULL, (void *)&name,
	                sizeof(name) + 1),
	    EINVAL, "thread.prof.name should reject a wrong newlen");
}
TEST_END

TEST_BEGIN(test_ctl_ro_macro_errors) {
	/* config.debug: a CTL_RO_CONFIG_GEN getter (read-only bool). */
	bool   dbg;
	size_t sz = sizeof(dbg);
	expect_d_eq(mallctl("config.debug", (void *)&dbg, &sz, (void *)&dbg,
	                sizeof(dbg)),
	    EPERM, "config.debug should reject a write");
	sz = sizeof(dbg) + 1;
	expect_d_eq(mallctl("config.debug", (void *)&dbg, &sz, NULL, 0), EINVAL,
	    "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(dbg), "Unexpected truncated oldlen");

	/* arenas.quantum: a CTL_RO_NL_GEN getter (read-only size_t). */
	size_t q;
	sz = sizeof(q);
	expect_d_eq(
	    mallctl("arenas.quantum", (void *)&q, &sz, (void *)&q, sizeof(q)),
	    EPERM, "arenas.quantum should reject a write");
	sz = sizeof(q) + 1;
	expect_d_eq(mallctl("arenas.quantum", (void *)&q, &sz, NULL, 0), EINVAL,
	    "Expected output size mismatch");
	expect_zu_eq(sz, sizeof(q), "Unexpected truncated oldlen");
}
TEST_END

TEST_BEGIN(test_arena_i_dirty_decay_ms) {
	ssize_t dirty_decay_ms, orig_dirty_decay_ms, prev_dirty_decay_ms;
	size_t  sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arena.0.dirty_decay_ms",
	                (void *)&orig_dirty_decay_ms, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	dirty_decay_ms = -2;
	expect_d_eq(mallctl("arena.0.dirty_decay_ms", NULL, NULL,
	                (void *)&dirty_decay_ms, sizeof(ssize_t)),
	    EFAULT, "Unexpected mallctl() success");

	dirty_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arena.0.dirty_decay_ms", NULL, NULL,
	                (void *)&dirty_decay_ms, sizeof(ssize_t)),
	    0, "Unexpected mallctl() failure");

	for (prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms = -1;
	    dirty_decay_ms < 20;
	    prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms++) {
		ssize_t old_dirty_decay_ms;

		expect_d_eq(mallctl("arena.0.dirty_decay_ms",
		                (void *)&old_dirty_decay_ms, &sz,
		                (void *)&dirty_decay_ms, sizeof(ssize_t)),
		    0, "Unexpected mallctl() failure");
		expect_zd_eq(old_dirty_decay_ms, prev_dirty_decay_ms,
		    "Unexpected old arena.0.dirty_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arena_i_muzzy_decay_ms) {
	ssize_t muzzy_decay_ms, orig_muzzy_decay_ms, prev_muzzy_decay_ms;
	size_t  sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arena.0.muzzy_decay_ms",
	                (void *)&orig_muzzy_decay_ms, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	muzzy_decay_ms = -2;
	expect_d_eq(mallctl("arena.0.muzzy_decay_ms", NULL, NULL,
	                (void *)&muzzy_decay_ms, sizeof(ssize_t)),
	    EFAULT, "Unexpected mallctl() success");

	muzzy_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arena.0.muzzy_decay_ms", NULL, NULL,
	                (void *)&muzzy_decay_ms, sizeof(ssize_t)),
	    0, "Unexpected mallctl() failure");

	for (prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms = -1;
	    muzzy_decay_ms < 20;
	    prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms++) {
		ssize_t old_muzzy_decay_ms;

		expect_d_eq(mallctl("arena.0.muzzy_decay_ms",
		                (void *)&old_muzzy_decay_ms, &sz,
		                (void *)&muzzy_decay_ms, sizeof(ssize_t)),
		    0, "Unexpected mallctl() failure");
		expect_zd_eq(old_muzzy_decay_ms, prev_muzzy_decay_ms,
		    "Unexpected old arena.0.muzzy_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arena_i_purge) {
	unsigned narenas;
	size_t   sz = sizeof(unsigned);
	size_t   mib[3];
	size_t   miblen = 3;

	expect_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlnametomib("arena.0.purge", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = narenas;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	mib[1] = MALLCTL_ARENAS_ALL;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}
TEST_END

TEST_BEGIN(test_arena_i_decay) {
	unsigned narenas;
	size_t   sz = sizeof(unsigned);
	size_t   mib[3];
	size_t   miblen = 3;

	expect_d_eq(mallctl("arena.0.decay", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_d_eq(mallctl("arenas.narenas", (void *)&narenas, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlnametomib("arena.0.decay", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = narenas;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");

	mib[1] = MALLCTL_ARENAS_ALL;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}
TEST_END

TEST_BEGIN(test_arena_i_dss) {
	const char *dss_prec_old, *dss_prec_new;
	size_t      sz = sizeof(dss_prec_old);
	size_t      mib[3];
	size_t      miblen;

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.dss", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");

	dss_prec_new = "disabled";
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz,
	                (void *)&dss_prec_new, sizeof(dss_prec_new)),
	    0, "Unexpected mallctl() failure");
	expect_str_ne(
	    dss_prec_old, "primary", "Unexpected default for dss precedence");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_new, &sz,
	                (void *)&dss_prec_old, sizeof(dss_prec_old)),
	    0, "Unexpected mallctl() failure");

	expect_d_eq(
	    mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_str_ne(
	    dss_prec_old, "primary", "Unexpected value for dss precedence");

	const char *dss_prec_invalid = "invalid";
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
	                (void *)&dss_prec_invalid, sizeof(dss_prec_invalid)),
	    EINVAL, "Expected invalid dss precedence failure");

	const char *dss_prec_current;
	sz = sizeof(dss_prec_current);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_current, &sz,
	                NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_str_eq(dss_prec_current, dss_prec_old,
	    "Invalid dss precedence changed current value");

	dss_prec_new = "disabled";
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
	                (void *)&dss_prec_new, sizeof(dss_prec_new) - 1),
	    EINVAL, "Expected dss precedence input size mismatch");

	sz = sizeof(dss_prec_current);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_current, &sz,
	                NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_str_eq(dss_prec_current, dss_prec_old,
	    "Bad dss precedence input size changed current value");

	mib[1] = narenas_total_get();
	dss_prec_new = "disabled";
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz,
	                (void *)&dss_prec_new, sizeof(dss_prec_new)),
	    0, "Unexpected mallctl() failure");
	expect_str_ne(
	    dss_prec_old, "primary", "Unexpected default for dss precedence");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&dss_prec_new, &sz,
	                (void *)&dss_prec_old, sizeof(dss_prec_new)),
	    0, "Unexpected mallctl() failure");

	expect_d_eq(
	    mallctlbymib(mib, miblen, (void *)&dss_prec_old, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_str_ne(
	    dss_prec_old, "primary", "Unexpected value for dss precedence");
}
TEST_END

TEST_BEGIN(test_arena_i_name) {
	unsigned    arena_ind;
	size_t      ind_sz = sizeof(arena_ind);
	size_t      mib[3];
	size_t      miblen;
	char        name_old[ARENA_NAME_LEN];
	char       *name_oldp = name_old;
	size_t      sz = sizeof(name_oldp);
	char        default_name[ARENA_NAME_LEN];
	const char *name_new = "test name";
	const char *super_long_name = "A name longer than ARENA_NAME_LEN";
	size_t      super_long_name_len = strlen(super_long_name);
	assert(super_long_name_len > ARENA_NAME_LEN);

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.name", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() error");

	expect_d_eq(
	    mallctl("arenas.create", (void *)&arena_ind, &ind_sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	mib[1] = arena_ind;

	malloc_snprintf(
	    default_name, sizeof(default_name), "manual_%u", arena_ind);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz,
	                (void *)&name_new, sizeof(name_new)),
	    0, "Unexpected mallctl() failure");
	expect_str_eq(
	    name_old, default_name, "Unexpected default value for arena name");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz,
	                (void *)&super_long_name, sizeof(super_long_name)),
	    0, "Unexpected mallctl() failure");
	expect_str_eq(name_old, name_new, "Unexpected value for arena name");

	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	int cmp = strncmp(name_old, super_long_name, ARENA_NAME_LEN - 1);
	expect_true(cmp == 0, "Unexpected value for long arena name ");

	const char *name_preserved = "preserved name";
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
	    (void *)&name_preserved, sizeof(name_preserved)), 0,
	    "Unexpected mallctl() failure");

	const char *name_bad_oldlen = "bad oldlen update";
	sz = sizeof(name_oldp) - 1;
	memset(name_old, 'x', sizeof(name_old));
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz,
	    (void *)&name_bad_oldlen, sizeof(name_bad_oldlen)), EINVAL,
	    "Unexpected mallctl() success");
	expect_zu_eq(sz, sizeof(name_oldp) - 1,
	    "Unexpected oldlen update");
	expect_c_eq(name_old[0], 'x', "Unexpected output write");

	sz = sizeof(name_oldp);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	expect_str_eq(name_old, name_preserved,
	    "Malformed oldlen write should not change arena name");

	const char *name_bad_newlen = "bad newlen update";
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
	    (void *)&name_bad_newlen, sizeof(name_bad_newlen) - 1), EINVAL,
	    "Unexpected mallctl() success");

	sz = sizeof(name_oldp);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	expect_str_eq(name_old, name_preserved,
	    "Malformed newlen write should not change arena name");

	char *name_null = NULL;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&name_null,
	    sizeof(name_null)), EINVAL, "Unexpected mallctl() success");

	sz = sizeof(name_oldp);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&name_oldp, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");
	expect_str_eq(name_old, name_preserved,
	    "Null name write should not change arena name");
}
TEST_END

TEST_BEGIN(test_arena_i_retain_grow_limit) {
	size_t old_limit, new_limit, default_limit;
	size_t mib[3];
	size_t miblen;

	bool   retain_enabled;
	size_t sz = sizeof(retain_enabled);
	expect_d_eq(mallctl("opt.retain", &retain_enabled, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	test_skip_if(!retain_enabled);

	sz = sizeof(default_limit);
	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.retain_grow_limit", mib, &miblen),
	    0, "Unexpected mallctlnametomib() error");

	expect_d_eq(mallctlbymib(mib, miblen, &default_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(default_limit, SC_LARGE_MAXCLASS,
	    "Unexpected default for retain_grow_limit");

	new_limit = PAGE - 1;
	expect_d_eq(mallctlbymib(
	                mib, miblen, NULL, NULL, &new_limit, sizeof(new_limit)),
	    EFAULT, "Unexpected mallctl() success");

	new_limit = PAGE + 1;
	expect_d_eq(mallctlbymib(
	                mib, miblen, NULL, NULL, &new_limit, sizeof(new_limit)),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlbymib(mib, miblen, &old_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(old_limit, PAGE, "Unexpected value for retain_grow_limit");

	/* Expect grow less than psize class 10. */
	new_limit = sz_pind2sz(10) - 1;
	expect_d_eq(mallctlbymib(
	                mib, miblen, NULL, NULL, &new_limit, sizeof(new_limit)),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctlbymib(mib, miblen, &old_limit, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(
	    old_limit, sz_pind2sz(9), "Unexpected value for retain_grow_limit");

	/* Restore to default. */
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, &default_limit,
	                sizeof(default_limit)),
	    0, "Unexpected mallctl() failure");
}
TEST_END

TEST_BEGIN(test_arenas_dirty_decay_ms) {
	ssize_t dirty_decay_ms, orig_dirty_decay_ms, prev_dirty_decay_ms;
	size_t  sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arenas.dirty_decay_ms",
	                (void *)&orig_dirty_decay_ms, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	dirty_decay_ms = -2;
	expect_d_eq(mallctl("arenas.dirty_decay_ms", NULL, NULL,
	                (void *)&dirty_decay_ms, sizeof(ssize_t)),
	    EFAULT, "Unexpected mallctl() success");

	dirty_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arenas.dirty_decay_ms", NULL, NULL,
	                (void *)&dirty_decay_ms, sizeof(ssize_t)),
	    0, "Expected mallctl() failure");

	for (prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms = -1;
	    dirty_decay_ms < 20;
	    prev_dirty_decay_ms = dirty_decay_ms, dirty_decay_ms++) {
		ssize_t old_dirty_decay_ms;

		expect_d_eq(mallctl("arenas.dirty_decay_ms",
		                (void *)&old_dirty_decay_ms, &sz,
		                (void *)&dirty_decay_ms, sizeof(ssize_t)),
		    0, "Unexpected mallctl() failure");
		expect_zd_eq(old_dirty_decay_ms, prev_dirty_decay_ms,
		    "Unexpected old arenas.dirty_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arenas_muzzy_decay_ms) {
	ssize_t muzzy_decay_ms, orig_muzzy_decay_ms, prev_muzzy_decay_ms;
	size_t  sz = sizeof(ssize_t);

	expect_d_eq(mallctl("arenas.muzzy_decay_ms",
	                (void *)&orig_muzzy_decay_ms, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	muzzy_decay_ms = -2;
	expect_d_eq(mallctl("arenas.muzzy_decay_ms", NULL, NULL,
	                (void *)&muzzy_decay_ms, sizeof(ssize_t)),
	    EFAULT, "Unexpected mallctl() success");

	muzzy_decay_ms = 0x7fffffff;
	expect_d_eq(mallctl("arenas.muzzy_decay_ms", NULL, NULL,
	                (void *)&muzzy_decay_ms, sizeof(ssize_t)),
	    0, "Expected mallctl() failure");

	for (prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms = -1;
	    muzzy_decay_ms < 20;
	    prev_muzzy_decay_ms = muzzy_decay_ms, muzzy_decay_ms++) {
		ssize_t old_muzzy_decay_ms;

		expect_d_eq(mallctl("arenas.muzzy_decay_ms",
		                (void *)&old_muzzy_decay_ms, &sz,
		                (void *)&muzzy_decay_ms, sizeof(ssize_t)),
		    0, "Unexpected mallctl() failure");
		expect_zd_eq(old_muzzy_decay_ms, prev_muzzy_decay_ms,
		    "Unexpected old arenas.muzzy_decay_ms");
	}
}
TEST_END

TEST_BEGIN(test_arenas_constants) {
#define TEST_ARENAS_CONSTANT(t, name, expected)                                \
	do {                                                                   \
		t      name;                                                   \
		size_t sz = sizeof(t);                                         \
		expect_d_eq(                                                   \
		    mallctl("arenas." #name, (void *)&name, &sz, NULL, 0), 0,  \
		    "Unexpected mallctl() failure");                           \
		expect_zu_eq(name, expected, "Incorrect " #name " size");      \
	} while (0)

	TEST_ARENAS_CONSTANT(size_t, quantum, QUANTUM);
	TEST_ARENAS_CONSTANT(size_t, page, PAGE);
	TEST_ARENAS_CONSTANT(size_t, hugepage, HUGEPAGE);
	TEST_ARENAS_CONSTANT(unsigned, nbins, SC_NBINS);
	TEST_ARENAS_CONSTANT(unsigned, nlextents, SC_NSIZES - SC_NBINS);

#undef TEST_ARENAS_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_bin_constants) {
#define TEST_ARENAS_BIN_CONSTANT(t, name, expected)                            \
	do {                                                                   \
		t      name;                                                   \
		size_t sz = sizeof(t);                                         \
		expect_d_eq(mallctl("arenas.bin.0." #name, (void *)&name, &sz, \
		                NULL, 0),                                      \
		    0, "Unexpected mallctl() failure");                        \
		expect_zu_eq(name, expected, "Incorrect " #name " size");      \
	} while (0)

	TEST_ARENAS_BIN_CONSTANT(size_t, size, bin_infos[0].reg_size);
	TEST_ARENAS_BIN_CONSTANT(uint32_t, nregs, bin_infos[0].nregs);
	TEST_ARENAS_BIN_CONSTANT(size_t, slab_size, bin_infos[0].slab_size);
	TEST_ARENAS_BIN_CONSTANT(uint32_t, nshards, bin_infos[0].n_shards);

#undef TEST_ARENAS_BIN_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_bin_oob) {
	size_t sz;
	size_t result;
	char   buf[128];

	/*
	 * Querying the bin at index SC_NBINS should fail because valid
	 * indices are [0, SC_NBINS).
	 */
	sz = sizeof(result);
	malloc_snprintf(
	    buf, sizeof(buf), "arenas.bin.%u.size", (unsigned)SC_NBINS);
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), ENOENT,
	    "mallctl() should fail for out-of-bounds bin index SC_NBINS");

	/* One below the boundary should succeed. */
	malloc_snprintf(
	    buf, sizeof(buf), "arenas.bin.%u.size", (unsigned)(SC_NBINS - 1));
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), 0,
	    "mallctl() should succeed for valid bin index SC_NBINS-1");
}
TEST_END

TEST_BEGIN(test_arenas_lextent_oob) {
	size_t   sz;
	size_t   result;
	char     buf[128];
	unsigned nlextents = SC_NSIZES - SC_NBINS;

	/*
	 * Querying the lextent at index nlextents should fail because valid
	 * indices are [0, nlextents).
	 */
	sz = sizeof(result);
	malloc_snprintf(buf, sizeof(buf), "arenas.lextent.%u.size", nlextents);
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), ENOENT,
	    "mallctl() should fail for out-of-bounds lextent index");

	/* Querying the last element (nlextents - 1) should succeed. */
	malloc_snprintf(
	    buf, sizeof(buf), "arenas.lextent.%u.size", nlextents - 1);
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), 0,
	    "mallctl() should succeed for valid lextent index");
}
TEST_END

TEST_BEGIN(test_stats_arenas_bins_oob) {
	test_skip_if(!config_stats);
	size_t   sz;
	uint64_t result;
	char     buf[128];

	uint64_t epoch = 1;
	sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sz), 0,
	    "Unexpected mallctl() failure");

	/* SC_NBINS is one past the valid range. */
	sz = sizeof(result);
	malloc_snprintf(buf, sizeof(buf), "stats.arenas.0.bins.%u.nmalloc",
	    (unsigned)SC_NBINS);
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), ENOENT,
	    "mallctl() should fail for out-of-bounds stats bin index");

	/* SC_NBINS - 1 is valid. */
	malloc_snprintf(buf, sizeof(buf), "stats.arenas.0.bins.%u.nmalloc",
	    (unsigned)(SC_NBINS - 1));
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), 0,
	    "mallctl() should succeed for valid stats bin index");
}
TEST_END

TEST_BEGIN(test_stats_arenas_lextents_oob) {
	test_skip_if(!config_stats);
	size_t   sz;
	uint64_t result;
	char     buf[128];
	unsigned nlextents = SC_NSIZES - SC_NBINS;

	uint64_t epoch = 1;
	sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sz), 0,
	    "Unexpected mallctl() failure");

	/* nlextents is one past the valid range. */
	sz = sizeof(result);
	malloc_snprintf(
	    buf, sizeof(buf), "stats.arenas.0.lextents.%u.nmalloc", nlextents);
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), ENOENT,
	    "mallctl() should fail for out-of-bounds stats lextent index");

	/* nlextents - 1 is valid. */
	malloc_snprintf(buf, sizeof(buf), "stats.arenas.0.lextents.%u.nmalloc",
	    nlextents - 1);
	expect_d_eq(mallctl(buf, (void *)&result, &sz, NULL, 0), 0,
	    "mallctl() should succeed for valid stats lextent index");
}
TEST_END

TEST_BEGIN(test_arenas_lextent_constants) {
#define TEST_ARENAS_LEXTENT_CONSTANT(t, name, expected)                        \
	do {                                                                   \
		t      name;                                                   \
		size_t sz = sizeof(t);                                         \
		expect_d_eq(mallctl("arenas.lextent.0." #name, (void *)&name,  \
		                &sz, NULL, 0),                                 \
		    0, "Unexpected mallctl() failure");                        \
		expect_zu_eq(name, expected, "Incorrect " #name " size");      \
	} while (0)

	TEST_ARENAS_LEXTENT_CONSTANT(size_t, size, SC_LARGE_MINCLASS);

#undef TEST_ARENAS_LEXTENT_CONSTANT
}
TEST_END

TEST_BEGIN(test_arenas_create) {
	unsigned narenas_before, arena, narenas_after;
	size_t   sz = sizeof(unsigned);

	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_before, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_u_eq(narenas_before + 1, narenas_after,
	    "Unexpected number of arenas before versus after extension");
	expect_u_eq(arena, narenas_after - 1, "Unexpected arena index");

	sz = sizeof(narenas_before);
	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_before, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(arena) - 1;
	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0),
	    EINVAL, "Unexpected mallctl() success");
	expect_zu_eq(sz, 0, "Unexpected oldlen update");

	sz = sizeof(narenas_after);
	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(narenas_before, narenas_after,
	    "Malformed oldlen should not create an arena");

	extent_hooks_t *hooks = NULL;
	sz = sizeof(arena);
	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz,
	    (void *)&hooks, sizeof(hooks) - 1), EINVAL,
	    "Unexpected mallctl() success");

	sz = sizeof(narenas_after);
	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(narenas_before, narenas_after,
	    "Malformed newlen should not create an arena");
}
TEST_END

TEST_BEGIN(test_experimental_arenas_create_ext_errors) {
	unsigned       narenas_before, arena, narenas_after;
	size_t         sz = sizeof(unsigned);
	arena_config_t config = arena_config_default;

	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_before, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	sz = sizeof(arena) - 1;
	expect_d_eq(mallctl("experimental.arenas_create_ext", (void *)&arena,
	    &sz, (void *)&config, sizeof(config)), EINVAL,
	    "Unexpected mallctl() success");
	expect_zu_eq(sz, 0, "Unexpected oldlen update");

	sz = sizeof(narenas_after);
	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(narenas_before, narenas_after,
	    "Malformed oldlen should not create an arena");

	sz = sizeof(arena);
	expect_d_eq(mallctl("experimental.arenas_create_ext", (void *)&arena,
	    &sz, (void *)&config, sizeof(config) - 1), EINVAL,
	    "Unexpected mallctl() success");

	sz = sizeof(narenas_after);
	expect_d_eq(
	    mallctl("arenas.narenas", (void *)&narenas_after, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(narenas_before, narenas_after,
	    "Malformed newlen should not create an arena");
}
TEST_END

TEST_BEGIN(test_arenas_lookup) {
	unsigned arena, arena1;
	void    *ptr;
	size_t   sz = sizeof(unsigned);

	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	ptr = mallocx(42, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(ptr, "Unexpected mallocx() failure");
	expect_d_eq(mallctl("arenas.lookup", &arena1, &sz, &ptr, sizeof(ptr)),
	    0, "Unexpected mallctl() failure");
	expect_u_eq(arena, arena1, "Unexpected arena index");

	expect_d_eq(
	    mallctl("arenas.lookup", &arena1, &sz, &ptr, sizeof(ptr) - 1),
	    EINVAL, "Unexpected mallctl() success");

	int stack_value;
	void *stack_ptr = &stack_value;
	expect_d_eq(mallctl("arenas.lookup", &arena1, &sz, &stack_ptr,
	    sizeof(stack_ptr)), EINVAL, "Unexpected mallctl() success");

	unsigned char short_old[sizeof(unsigned)];
	memset(short_old, 0, sizeof(short_old));
	sz = sizeof(unsigned) - 1;
	expect_d_eq(mallctl("arenas.lookup", short_old, &sz, &ptr, sizeof(ptr)),
	    EINVAL, "Unexpected mallctl() success");
	expect_zu_eq(sz, sizeof(unsigned) - 1, "Unexpected oldlen update");
	expect_d_eq(memcmp(short_old, &arena, sizeof(unsigned) - 1), 0,
	    "Unexpected partial arena index");

	unsigned char long_old[sizeof(unsigned) + 1];
	memset(long_old, 0, sizeof(long_old));
	long_old[sizeof(unsigned)] = 0x5a;
	sz = sizeof(long_old);
	expect_d_eq(mallctl("arenas.lookup", long_old, &sz, &ptr, sizeof(ptr)),
	    EINVAL, "Unexpected mallctl() success");
	expect_zu_eq(sz, sizeof(unsigned), "Unexpected oldlen update");
	expect_d_eq(memcmp(long_old, &arena, sizeof(unsigned)), 0,
	    "Unexpected partial arena index");
	expect_u_eq(long_old[sizeof(unsigned)], 0x5a,
	    "Unexpected write past arena index");

	dallocx(ptr, 0);
}
TEST_END

TEST_BEGIN(test_prof_active) {
	/*
	 * If config_prof is off, then the test for prof_active in
	 * test_mallctl_opt was already enough.
	 */
	test_skip_if(!config_prof);
	test_skip_if(opt_prof);

	bool   active, old;
	size_t len = sizeof(bool);

	active = true;
	expect_d_eq(mallctl("prof.active", NULL, NULL, &active, len), ENOENT,
	    "Setting prof_active to true should fail when opt_prof is off");
	old = true;
	expect_d_eq(mallctl("prof.active", &old, &len, &active, len), ENOENT,
	    "Setting prof_active to true should fail when opt_prof is off");
	expect_true(old, "old value should not be touched when mallctl fails");
	active = false;
	expect_d_eq(mallctl("prof.active", NULL, NULL, &active, len), 0,
	    "Setting prof_active to false should succeed when opt_prof is off");
	expect_d_eq(mallctl("prof.active", &old, &len, &active, len), 0,
	    "Setting prof_active to false should succeed when opt_prof is off");
	expect_false(old, "prof_active should be false when opt_prof is off");
}
TEST_END

TEST_BEGIN(test_stats_arenas) {
#define TEST_STATS_ARENAS(t, name)                                             \
	do {                                                                   \
		t      name;                                                   \
		size_t sz = sizeof(t);                                         \
		expect_d_eq(mallctl("stats.arenas.0." #name, (void *)&name,    \
		                &sz, NULL, 0),                                 \
		    0, "Unexpected mallctl() failure");                        \
	} while (0)

	TEST_STATS_ARENAS(unsigned, nthreads);
	TEST_STATS_ARENAS(const char *, dss);
	TEST_STATS_ARENAS(ssize_t, dirty_decay_ms);
	TEST_STATS_ARENAS(ssize_t, muzzy_decay_ms);
	TEST_STATS_ARENAS(size_t, pactive);
	TEST_STATS_ARENAS(size_t, pdirty);

#undef TEST_STATS_ARENAS
}
TEST_END

TEST_BEGIN(test_stats_arenas_hpa_shard_counters) {
	test_skip_if(!config_stats);

#define TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(t, name)                          \
	do {                                                                   \
		t      name;                                                   \
		size_t sz = sizeof(t);                                         \
		expect_d_eq(mallctl("stats.arenas.0.hpa_shard." #name,         \
		                (void *)&name, &sz, NULL, 0),                  \
		    0, "Unexpected mallctl() failure");                        \
	} while (0)

	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(size_t, npageslabs);
	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(size_t, nactive);
	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(size_t, ndirty);
	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(uint64_t, npurge_passes);
	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(uint64_t, npurges);
	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(uint64_t, nhugifies);
	TEST_STATS_ARENAS_HPA_SHARD_COUNTERS(uint64_t, ndehugifies);

#undef TEST_STATS_ARENAS_HPA_SHARD_COUNTERS
}
TEST_END

TEST_BEGIN(test_stats_arenas_hpa_shard_slabs) {
	test_skip_if(!config_stats);

#define TEST_STATS_ARENAS_HPA_SHARD_SLABS_GEN(t, slab, name)                   \
	do {                                                                   \
		t      slab##_##name;                                          \
		size_t sz = sizeof(t);                                         \
		expect_d_eq(                                                   \
		    mallctl("stats.arenas.0.hpa_shard." #slab "." #name,       \
		        (void *)&slab##_##name, &sz, NULL, 0),                 \
		    0, "Unexpected mallctl() failure");                        \
	} while (0)

#define TEST_STATS_ARENAS_HPA_SHARD_SLABS(t, slab, name)                       \
	do {                                                                   \
		TEST_STATS_ARENAS_HPA_SHARD_SLABS_GEN(                         \
		    t, slab, name##_##nonhuge);                                \
		TEST_STATS_ARENAS_HPA_SHARD_SLABS_GEN(t, slab, name##_##huge); \
	} while (0)

	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, slabs, npageslabs);
	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, slabs, nactive);
	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, slabs, ndirty);

	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, full_slabs, npageslabs);
	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, full_slabs, nactive);
	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, full_slabs, ndirty);

	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, empty_slabs, npageslabs);
	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, empty_slabs, nactive);
	TEST_STATS_ARENAS_HPA_SHARD_SLABS(size_t, empty_slabs, ndirty);

#undef TEST_STATS_ARENAS_HPA_SHARD_SLABS
#undef TEST_STATS_ARENAS_HPA_SHARD_SLABS_GEN
}
TEST_END

TEST_BEGIN(test_thread_idle) {
	/*
	 * We're cheating a little bit in this test, and inferring things about
	 * implementation internals (like tcache details).  We have to;
	 * thread.idle has no guaranteed effects.  We need stats to make these
	 * inferences.
	 */
	test_skip_if(!config_stats);

	int    err;
	size_t sz;
	size_t miblen;

	bool tcache_enabled = false;
	sz = sizeof(tcache_enabled);
	err = mallctl("thread.tcache.enabled", &tcache_enabled, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	test_skip_if(!tcache_enabled);

	size_t tcache_max;
	sz = sizeof(tcache_max);
	err = mallctl("arenas.tcache_max", &tcache_max, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	test_skip_if(tcache_max == 0);

	unsigned arena_ind;
	sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	err = mallctl(
	    "thread.arena", NULL, NULL, &arena_ind, sizeof(arena_ind));
	expect_d_eq(err, 0, "Unexpected mallctl() failure");
	err = mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "Unexpected mallctl() failure");

	/* We're going to do an allocation of size 1, which we know is small. */
	size_t mib[5];
	miblen = sizeof(mib) / sizeof(mib[0]);
	err = mallctlnametomib("stats.arenas.0.small.ndalloc", mib, &miblen);
	expect_d_eq(err, 0, "");
	mib[2] = arena_ind;

	/*
	 * This alloc and dalloc should leave something (from the newly created
	 * arena) in the tcache, in a small size's cache bin.  Later the stats
	 * of that arena will be checked to verify if tcache flush happened.
	 */
	void *ptr = mallocx(1, MALLOCX_TCACHE_NONE);
	dallocx(ptr, 0);

	uint64_t epoch;
	err = mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch));
	expect_d_eq(err, 0, "");

	uint64_t small_dalloc_pre_idle;
	sz = sizeof(small_dalloc_pre_idle);
	err = mallctlbymib(mib, miblen, &small_dalloc_pre_idle, &sz, NULL, 0);
	expect_d_eq(err, 0, "");

	err = mallctl("thread.idle", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "");

	err = mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch));
	expect_d_eq(err, 0, "");

	uint64_t small_dalloc_post_idle;
	sz = sizeof(small_dalloc_post_idle);
	err = mallctlbymib(mib, miblen, &small_dalloc_post_idle, &sz, NULL, 0);
	expect_d_eq(err, 0, "");

	expect_u64_lt(small_dalloc_pre_idle, small_dalloc_post_idle,
	    "Purge didn't flush the tcache");
}
TEST_END

TEST_BEGIN(test_thread_peak) {
	test_skip_if(!config_stats);

	/*
	 * We don't commit to any stable amount of accuracy for peak tracking
	 * (in practice, when this test was written, we made sure to be within
	 * 100k).  But 10MB is big for more or less any definition of big.
	 */
	size_t big_size = 10 * 1024 * 1024;
	size_t small_size = 256;

	void    *ptr;
	int      err;
	size_t   sz;
	uint64_t peak;
	sz = sizeof(uint64_t);

	err = mallctl("thread.peak.reset", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "");
	ptr = mallocx(SC_SMALL_MAXCLASS, 0);
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_eq(peak, SC_SMALL_MAXCLASS, "Missed an update");
	free(ptr);
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_eq(peak, SC_SMALL_MAXCLASS, "Freeing changed peak");
	ptr = mallocx(big_size, 0);
	free(ptr);
	/*
	 * The peak should have hit big_size in the last two lines, even though
	 * the net allocated bytes has since dropped back down to zero.  We
	 * should have noticed the peak change without having down any mallctl
	 * calls while net allocated bytes was high.
	 */
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_ge(peak, big_size, "Missed a peak change.");

	/* Allocate big_size, but using small allocations. */
	size_t nallocs = big_size / small_size;
	void **ptrs = calloc(nallocs, sizeof(void *));
	err = mallctl("thread.peak.reset", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "");
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	expect_u64_eq(0, peak, "Missed a reset.");
	for (size_t i = 0; i < nallocs; i++) {
		ptrs[i] = mallocx(small_size, 0);
	}
	for (size_t i = 0; i < nallocs; i++) {
		free(ptrs[i]);
	}
	err = mallctl("thread.peak.read", &peak, &sz, NULL, 0);
	expect_d_eq(err, 0, "");
	/*
	 * We don't guarantee exactness; make sure we're within 10% of the peak,
	 * though.
	 */
	expect_u64_ge(peak, nallocx(small_size, 0) * nallocs * 9 / 10,
	    "Missed some peak changes.");
	expect_u64_le(peak, nallocx(small_size, 0) * nallocs * 11 / 10,
	    "Overcounted peak changes.");
	free(ptrs);
}
TEST_END

static unsigned nuser_thread_event_cb_calls;
static void
user_thread_event_cb(bool is_alloc, uint64_t tallocated, uint64_t tdallocated) {
	(void)tdallocated;
	(void)tallocated;
	++nuser_thread_event_cb_calls;
}
static user_hook_object_t user_te_obj = {
    .callback = user_thread_event_cb,
    .interval = 100,
    .is_alloc_only = false,
};

TEST_BEGIN(test_thread_event_hook) {
	const size_t big_size = 10 * 1024 * 1024;
	void        *ptr;
	int          err;

	unsigned current_calls = nuser_thread_event_cb_calls;
	err = mallctl("experimental.hooks.thread_event", NULL, 0, &user_te_obj,
	    sizeof(user_te_obj));
	assert_d_eq(0, err, "");

	err = mallctl("experimental.hooks.thread_event", NULL, 0, &user_te_obj,
	    sizeof(user_te_obj));
	assert_d_eq(
	    0, err, "Not an error to provide object with same interval and cb");

	ptr = mallocx(big_size, 0);
	free(ptr);
	expect_u64_lt(current_calls, nuser_thread_event_cb_calls, "");
}
TEST_END

int
main(void) {
	return test(test_mallctl_errors, test_mallctlnametomib_errors,
	    test_mallctlbymib_errors, test_ctl_mib_unsigned,
	    test_ctl_verify_read, test_ctl_readonly,
	    test_ctl_neither_read_nor_write, test_ctl_read_xor_write,
	    test_mallctl_read_write,
	    test_mallctl_read_partial, test_tcache_create_errors,
	    test_mallctlnametomib_short_mib, test_mallctlnametomib_short_name,
	    test_mallctlmibnametomib, test_mallctlbymibname,
	    test_mallctl_config, test_mallctl_opt, test_manpage_example,
	    test_tcache_none, test_tcache, test_thread_arena,
	    test_thread_arena_bad_oldlen_no_migrate, test_arena_i_initialized,
	    test_arena_i_initialized_errors,
	    test_thread_tcache_enabled_errors,
	    test_thread_peak_read_errors,
	    test_arenas_narenas_errors, test_approximate_stats_active_errors,
	    test_decay_ms_oversize_errors, test_background_thread_errors,
	    test_prof_toggle_errors,
	    test_experimental_prof_recent_alloc_max_errors,
	    test_prof_stats_errors, test_thread_tcache_flush_errors,
	    test_thread_peak_reset_errors, test_thread_idle_errors,
	    test_arena_i_decay_errors, test_arena_i_purge_errors,
	    test_arena_i_reset_destroy_errors, test_thread_prof_name_errors,
	    test_ctl_ro_macro_errors,
	    test_arena_i_dirty_decay_ms, test_arena_i_muzzy_decay_ms,
	    test_arena_i_purge, test_arena_i_decay, test_arena_i_dss,
	    test_arena_i_name, test_arena_i_retain_grow_limit,
	    test_arenas_dirty_decay_ms, test_arenas_muzzy_decay_ms,
	    test_arenas_constants, test_arenas_bin_constants,
	    test_arenas_bin_oob, test_arenas_lextent_oob,
	    test_stats_arenas_bins_oob, test_stats_arenas_lextents_oob,
	    test_arenas_lextent_constants, test_arenas_create,
	    test_experimental_arenas_create_ext_errors, test_arenas_lookup,
	    test_prof_active, test_stats_arenas,
	    test_stats_arenas_hpa_shard_counters,
	    test_stats_arenas_hpa_shard_slabs, test_thread_idle, test_thread_peak,
	    test_thread_event_hook);
}
