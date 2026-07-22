#ifndef AIMEE_HTTP_CONTENT_ENCODING_H
#define AIMEE_HTTP_CONTENT_ENCODING_H

#include <stddef.h>

/* RFC 1952 gzip helpers. Returned buffers are heap-owned. Decompression is
 * bounded by both max_output and max_ratio (plus a fixed framing allowance).
 * -2 reports a limit violation, -1 malformed/OOM, and 0 success. */
int http_content_encoding_available(void);
int http_gzip_compress(const void *input, size_t input_len, unsigned char **output,
                       size_t *output_len);
int http_gzip_decompress(const void *input, size_t input_len, size_t max_output, size_t max_ratio,
                         unsigned char **output, size_t *output_len);

#endif
