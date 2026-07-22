#ifndef AIMEE_TEST_AWS_EVENTSTREAM_FIXTURE_H
#define AIMEE_TEST_AWS_EVENTSTREAM_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
   const char *name;
   uint8_t value_type;
   const void *value;
   size_t value_len;
   int64_t int_value;
} test_aws_es_header_t;

/* Build a CRC-valid eventstream message from an arbitrary header list. This is
 * deliberately permissive so semantic-boundary tests can supply duplicate
 * reserved headers, wrong value types, and embedded NUL bytes while keeping the
 * binary framing itself valid. */
int test_aws_es_message(const test_aws_es_header_t *headers, size_t n_headers, const void *payload,
                        size_t payload_len, uint8_t **out, size_t *out_len);

/* Build a CRC-valid Bedrock event frame with the three required string headers.
 * The returned buffer is heap-owned and must be released with the matching free. */
int test_aws_es_event(const char *event_type, const char *payload, uint8_t **out, size_t *out_len);
int test_aws_es_exception(const char *exception_type, const char *payload, uint8_t **out,
                          size_t *out_len);
int test_aws_es_error(const char *code, const char *message, uint8_t **out, size_t *out_len);
void test_aws_es_fixture_free(uint8_t **buffer, size_t length);

#endif
