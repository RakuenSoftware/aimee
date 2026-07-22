#include <stdio.h>
#include <string.h>
#include "../kb/kb_mgmt_client.h"

int main(int argc, char **argv)
{
   if (argc != 3)
   {
      fprintf(stderr, "usage: %s https://host:port ca.pem\n", argv[0]);
      return 2;
   }
   FILE *f = fopen(argv[2], "rb");
   if (!f)
      return 2;
   char ca[65536];
   size_t nca = fread(ca, 1, sizeof(ca) - 1, f);
   fclose(f);
   ca[nca] = 0;
   char out[4096];
   int status = 0;
   const char *auth = "Authorization: Bearer management-test\r\n";
   int rc = kb_mgmt_client_request_auth(argv[1], ca, NULL, NULL, "GET", "/v1/health", NULL, auth,
                                        out, sizeof(out), &status);
   if (rc != 0 || status != 200 || !strstr(out, "ok"))
      return 1;
   puts("kb_mgmt_live: ok");
   return 0;
}
