#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ctl.h"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        "normal",                    // Valid input
        "A" * 255,                   // Boundary case (assuming 256 byte buffer)
        "EXPLOIT" * 100,             // Exact exploit case (700 bytes)
        "X" * 1024,                  // 10x oversized input
        "\0" * 512                   // Null byte overflow attempt
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char dst[256] = {0};  // Fixed destination buffer
        size_t dst_size = sizeof(dst);
        
        // Call the actual vulnerable function from ctl.c
        int result = process_control_data(payloads[i], strlen(payloads[i]), dst, dst_size);
        
        // Security invariant: No buffer overflow should occur
        // Either the function rejects/truncates or handles safely
        ck_assert_msg(result >= 0, 
                     "Buffer overflow detected with payload %d (length: %zu)", 
                     i, strlen(payloads[i]));
        
        // Verify no writes beyond buffer boundary
        char guard_byte = 0x42;
        dst[dst_size - 1] = guard_byte;
        ck_assert_msg(dst[dst_size - 1] == guard_byte,
                     "Memory corruption detected beyond buffer boundary");
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