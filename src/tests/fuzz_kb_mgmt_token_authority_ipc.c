#include "kb_mgmt_token_authority_ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define FUZZ_INPUT_MAX 4096u

int kb_mgmt_token_authority_ipc_read_request(int fd, uint32_t timeout_ms, char correlation_id[65],
                                             char jti[65]);

static void fuzz_one(const uint8_t *data, size_t size)
{
   if ((!data && size) || size > FUZZ_INPUT_MAX)
      return;
   int pair[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
      abort();
   size_t offset = 0;
   while (offset < size)
   {
      ssize_t n = write(pair[0], data + offset, size - offset);
      if (n <= 0)
         abort();
      offset += (size_t)n;
   }
   if (shutdown(pair[0], SHUT_WR) != 0)
      abort();
   char correlation_id[65] = "", jti[65] = "";
   int rc = kb_mgmt_token_authority_ipc_read_request(pair[1], 1, correlation_id, jti);
   if (rc == 0 && (correlation_id[64] != 0 || jti[64] != 0))
      abort();
   close(pair[0]);
   close(pair[1]);
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
   fuzz_one(data, size);
   return 0;
}
#else
static int fuzz_file(const char *path)
{
   FILE *file = fopen(path, "rb");
   if (!file)
      return 1;
   uint8_t input[FUZZ_INPUT_MAX];
   size_t size = fread(input, 1, sizeof(input), file);
   int too_large = size == sizeof(input) && fgetc(file) != EOF;
   int failed = ferror(file);
   fclose(file);
   if (!failed && !too_large)
      fuzz_one(input, size);
   return failed || too_large;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      uint8_t input[FUZZ_INPUT_MAX];
      fuzz_one(input, fread(input, 1, sizeof(input), stdin));
   }
   else
      for (int i = 1; i < argc; ++i)
         if (fuzz_file(argv[i]) != 0)
            return 1;
   printf("fuzz_kb_mgmt_token_authority_ipc: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
