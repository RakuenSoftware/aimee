#include "http_content_encoding.h"

/* Codec-less builds retain wire compatibility by never advertising gzip. */
int http_content_encoding_available(void)
{
   return 0;
}

int http_gzip_compress(const void *input, size_t input_len, unsigned char **output,
                       size_t *output_len)
{
   (void)input;
   (void)input_len;
   if (output)
      *output = NULL;
   if (output_len)
      *output_len = 0;
   return -1;
}

int http_gzip_decompress(const void *input, size_t input_len, size_t max_output, size_t max_ratio,
                         unsigned char **output, size_t *output_len)
{
   (void)input;
   (void)input_len;
   (void)max_output;
   (void)max_ratio;
   if (output)
      *output = NULL;
   if (output_len)
      *output_len = 0;
   return -1;
}
