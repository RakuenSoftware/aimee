#include "http_content_encoding.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int http_content_encoding_available(void)
{
   return 1;
}

int http_gzip_compress(const void *input, size_t input_len, unsigned char **output,
                       size_t *output_len)
{
   if (!output || !output_len || (!input && input_len) || input_len > UINT_MAX)
      return -1;
   *output = NULL;
   *output_len = 0;
   z_stream stream;
   memset(&stream, 0, sizeof(stream));
   if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                    Z_DEFAULT_STRATEGY) != Z_OK)
      return -1;
   size_t cap = (size_t)deflateBound(&stream, (uLong)input_len);
   if (cap > UINT_MAX)
   {
      deflateEnd(&stream);
      return -1;
   }
   unsigned char *buf = malloc(cap ? cap : 1);
   if (!buf)
   {
      deflateEnd(&stream);
      return -1;
   }
   stream.next_in = (Bytef *)input;
   stream.avail_in = (uInt)input_len;
   stream.next_out = buf;
   stream.avail_out = (uInt)cap;
   int rc = deflate(&stream, Z_FINISH);
   if (rc != Z_STREAM_END)
   {
      free(buf);
      deflateEnd(&stream);
      return -1;
   }
   *output_len = (size_t)stream.total_out;
   *output = buf;
   deflateEnd(&stream);
   return 0;
}

int http_gzip_decompress(const void *input, size_t input_len, size_t max_output, size_t max_ratio,
                         unsigned char **output, size_t *output_len)
{
   if (!output || !output_len || !input || !input_len || !max_output || !max_ratio ||
       input_len > UINT_MAX)
      return -1;
   *output = NULL;
   *output_len = 0;
   size_t ratio_limit =
       input_len > (SIZE_MAX - 1024) / max_ratio ? SIZE_MAX : input_len * max_ratio + 1024;
   size_t limit = max_output < ratio_limit ? max_output : ratio_limit;
   if (limit == SIZE_MAX || limit > UINT_MAX)
      return -1;
   unsigned char *buf = malloc(limit + 1);
   if (!buf)
      return -1;
   z_stream stream;
   memset(&stream, 0, sizeof(stream));
   if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK)
   {
      free(buf);
      return -1;
   }
   stream.next_in = (Bytef *)input;
   stream.avail_in = (uInt)input_len;
   stream.next_out = buf;
   stream.avail_out = (uInt)limit;
   int rc = inflate(&stream, Z_FINISH);
   int result = 0;
   if (rc != Z_STREAM_END || stream.avail_in != 0)
      result = stream.avail_out == 0 ? -2 : -1;
   if (result == 0)
   {
      buf[stream.total_out] = '\0';
      *output = buf;
      *output_len = (size_t)stream.total_out;
   }
   else
      free(buf);
   inflateEnd(&stream);
   return result;
}
