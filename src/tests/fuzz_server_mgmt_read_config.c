#include "management_read.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef FUZZ_STANDALONE
#include <stdio.h>
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   server_mgmt_read_config_t config = {0};
   char wire[512];
   size_t copy = size < sizeof(config) ? size : sizeof(config);

   memcpy(&config, data, copy);
   memset(wire, 0xa5, sizeof(wire));
   int result = server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire));
   if (result >= 0)
   {
      assert((size_t)result < sizeof(wire));
      assert(wire[result] == '\0');
      assert(strstr(wire, "\"server_id\":\"server-a\"") != NULL);
      assert(strstr(wire, "\"team\":42") != NULL);
      assert(strstr(wire, "api_key") == NULL);
   }
   else
      assert(wire[0] == '\0');
   return 0;
}

#ifdef FUZZ_STANDALONE
int main(void)
{
   uint8_t input[sizeof(server_mgmt_read_config_t)];
   uint32_t state = 0x6d676d74U;
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
   puts("fuzz_server_mgmt_read_config: 10000 inputs ok");
   return 0;
}
#endif
