#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "src/gateway/gateway_ctx.h"

START_TEST(test_stream_text_cb_security_invariant)
{
    // Invariant: stream_buf_t internal state remains consistent under adversarial inputs
    const char *payloads[] = {
        "A",                    // Valid minimal input
        "",                     // Empty string boundary
        "X",                    // Single character
        "AAAAAAAAAAAAAAAAAAAA"  // 20-char chunk to trigger realloc
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        // Setup test context
        stream_buf_t sb = {0};
        sb.buf = malloc(1);
        sb.cap = 1;
        sb.len = 0;
        sb.buf[0] = '\0';
        
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "text", payloads[i]);
        
        // Call production function
        int result = stream_text_cb(event, &sb);
        
        // Security property: If function returns 0 (including realloc failure case),
        // sb.buf must either be NULL or point to valid memory with null termination
        if (result == 0) {
            if (sb.buf != NULL) {
                // Buffer must be null-terminated within bounds
                ck_assert_msg(sb.buf[sb.len] == '\0', 
                    "Buffer not null-terminated after stream_text_cb");
                
                // Buffer length must not exceed capacity
                ck_assert_msg(sb.len < sb.cap, 
                    "Buffer length exceeds capacity after stream_text_cb");
            }
        }
        
        // Cleanup
        cJSON_Delete(event);
        free(sb.buf);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_stream_text_cb_security_invariant);
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