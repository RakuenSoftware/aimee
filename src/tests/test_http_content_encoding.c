#include "http_content_encoding.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
   size_t input_len = 32 * 1024;
   unsigned char *input = malloc(input_len);
   assert(input);
   for (size_t i = 0; i < input_len; i++)
      input[i] = (unsigned char)("aimee-transport-json"[i % 20]);

   unsigned char *wire = NULL;
   size_t wire_len = 0;
   assert(http_gzip_compress(input, input_len, &wire, &wire_len) == 0);
   assert(wire && wire_len < input_len && wire[0] == 0x1f && wire[1] == 0x8b);

   unsigned char *decoded = NULL;
   size_t decoded_len = 0;
   assert(http_gzip_decompress(wire, wire_len, 1u << 20, 1000, &decoded, &decoded_len) == 0);
   assert(decoded_len == input_len && memcmp(decoded, input, input_len) == 0);
   free(decoded);

   assert(http_gzip_decompress(wire, wire_len, 1024, 1000, &decoded, &decoded_len) == -2);
   assert(!decoded);
   assert(http_gzip_decompress(wire, wire_len, 1u << 20, 2, &decoded, &decoded_len) == -2);
   assert(!decoded);
   wire[wire_len / 2] ^= 0x80;
   assert(http_gzip_decompress(wire, wire_len, 1u << 20, 1000, &decoded, &decoded_len) == -1);
   assert(!decoded);

   free(wire);
   free(input);
   puts("http_content_encoding: ok");
   return 0;
}
