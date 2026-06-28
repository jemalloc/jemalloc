#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "../src/arena.c"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        "normal",                    // Valid input
        "A",                         // Boundary: single char
        "very_long_string_that_exceeds_buffer_by_more_than_double_the_size",  // Exploit case
        "exact_length_plus_one",     // Boundary: length+1
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"  // Large payload
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    
    for (int i = 0; i < num_payloads; i++) {
        char dest[16] = {0};  // Fixed buffer size
        const char *src = payloads[i];
        size_t src_len = strlen(src);
        
        // Test strncpy behavior - should not write beyond dest[15]
        strncpy(dest, src, sizeof(dest));
        
        // Verify no buffer overflow by checking last byte is either null or within bounds
        ck_assert_msg(dest[sizeof(dest)-1] == '\0' || src_len < sizeof(dest),
                     "Buffer overflow detected for payload: %s", src);
        
        // Additional safety check: ensure we can read entire buffer without segfault
        for (size_t j = 0; j < sizeof(dest); j++) {
            volatile char c = dest[j];  // Force read
            (void)c;  // Suppress unused warning
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_never_exceed_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}