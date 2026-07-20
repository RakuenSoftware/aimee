#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../kb/kb_bedrock_egress.h"

int main(int argc, char **argv)
{
   if (argc != 3) {
      fprintf(stderr, "usage: %s https://host:port ca.pem\n", argv[0]);
      return 2;
   }
   aimee_request_t ir = {0};
   ir.model = "live-test";
   kb_bedrock_target_t t = {"live-test", "us-east-1", "aws", argv[1]};
   FILE *f = fopen(argv[2], "rb");
   if (!f) return 2;
   char ca[65536];
   size_t ca_len = fread(ca, 1, sizeof(ca) - 1, f);
   fclose(f);
   ca[ca_len] = 0;
   char response[262144];
   int status = 0;
   int rc = kb_bedrock_dispatch_https(&t, &ir, 0, "AKID", "SECRET", NULL,
                                      "20260101T000000Z", "20260101", ca, NULL,
                                      response, sizeof(response), &status);
   if (rc != 0 || status != 200) {
      fprintf(stderr, "Bedrock mock request failed rc=%d status=%d\n", rc, status);
      return 1;
   }
   if (!strstr(response, "200") && !strstr(response, "ok") && !strstr(response, "HTTP"))
      return 1;
   puts("kb_bedrock_live: ok");
   return 0;
}
