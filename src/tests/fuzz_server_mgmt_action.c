#include "server/server_mgmt_endpoint.h"
#include <stddef.h>
#include <stdint.h>
#ifdef FUZZ_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   server_mgmt_action_t action;
   if (size <= SERVER_MGMT_ACTION_BODY_MAX)
      (void)server_mgmt_action_parse((const char *)data, size, &action);
   return 0;
}

#ifdef FUZZ_STANDALONE
int main(void)
{
   uint8_t input[SERVER_MGMT_ACTION_BODY_MAX];
   uint32_t state = 0x5eed1234U;
   for (size_t run = 0; run < 10000; run++)
   {
      size_t n = run % (sizeof(input) + 1);
      for (size_t i = 0; i < n; i++)
      {
         state = state * 1664525U + 1013904223U;
         input[i] = (uint8_t)(state >> 24);
      }
      LLVMFuzzerTestOneInput(input, n);
   }
   puts("fuzz_server_mgmt_action: 10000 inputs ok");
   return 0;
}
#endif
