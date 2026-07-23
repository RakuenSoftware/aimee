#include "kb_mgmt_status_authority.h"
#include "kb_mgmt_status_listener.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX 8192u

static void fuzz_one(const uint8_t *data, size_t size)
{
   if ((!data && size) || size > FUZZ_MAX)
      return;
   kb_mgmt_status_route_t route;
   const char *body;
   size_t body_len;
   (void)kb_mgmt_status_http_parse_route(data, size, &route, &body, &body_len);
   char *text = malloc(size + 1);
   if (!text)
      return;
   memcpy(text, data, size);
   text[size] = 0;
   kb_mgmt_checkpoint_request_t request;
   kb_mgmt_checkpoint_t checkpoint;
   (void)kb_mgmt_checkpoint_request_from_json(text, size, &request);
   (void)kb_mgmt_checkpoint_from_json(text, &checkpoint);
   free(text);
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   fuzz_one(data, size);
   return 0;
}
#else
int main(int argc, char **argv)
{
   unsigned char input[FUZZ_MAX];
   if (argc < 2)
      fuzz_one(input, fread(input, 1, sizeof(input), stdin));
   else
      for (int i = 1; i < argc; ++i)
      {
         FILE *f = fopen(argv[i], "rb");
         if (!f)
            return 1;
         size_t n = fread(input, 1, sizeof(input), f);
         fclose(f);
         fuzz_one(input, n);
      }
   printf("fuzz_kb_mgmt_checkpoint: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
