#include "kb/kb_management_action.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FUZZ_MAX 4096U

static void fuzz_one(const unsigned char *data, size_t size)
{
   if ((!data && size) || size > FUZZ_MAX)
      return;
   kb_management_action_body_t body;
   (void)kb_management_action_body_parse((const char *)data, size, &body);
   db2_management_action_outcome_operation_t outcome = {0};
   int status = size ? 100 + data[0] % 500 : 200;
   (void)kb_management_action_response_parse((const char *)data, size, status, &outcome);
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
   if (argc < 2)
   {
      unsigned char input[FUZZ_MAX];
      fuzz_one(input, fread(input, 1, sizeof(input), stdin));
   }
   else
   {
      for (int i = 1; i < argc; i++)
      {
         FILE *f = fopen(argv[i], "rb");
         if (!f)
            return 1;
         unsigned char input[FUZZ_MAX];
         size_t n = fread(input, 1, sizeof(input), f);
         fuzz_one(input, n);
         fclose(f);
      }
   }
   return 0;
}
#endif
