#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kb/kb_bedrock_egress.h"

static char seen_host[512], seen_path[1024], seen_auth[2400];
static int seen_port;

int kb_tls_client_request_auth(const char *host, int port, const char *ca,
                               const char *cert, const char *key,
                               const char *method, const char *path,
                               const char *body, const char *auth,
                               char *out, size_t cap, int *status)
{
   (void)ca; (void)cert; (void)key; (void)method; (void)body;
   snprintf(seen_host, sizeof(seen_host), "%s", host);
   snprintf(seen_path, sizeof(seen_path), "%s", path);
   snprintf(seen_auth, sizeof(seen_auth), "%s", auth);
   seen_port = port;
   snprintf(out, cap, "{\"ok\":true}");
   if (status) *status = 200;
   return 0;
}

int main(void)
{
   aimee_request_t ir = {0};
   ir.model = "demo";
   kb_bedrock_target_t t = {"demo.model", "us-east-1", "aws", "https://mockbedrock:8443/base"};
   char out[64]; int status = 0;
   assert(kb_bedrock_dispatch_https(&t, &ir, 0, "AKID", "SECRET", "TOKEN",
                                    "20260101T000000Z", "20260101", "CA", NULL,
                                    out, sizeof(out), &status) == 0);
   assert(strcmp(seen_host, "mockbedrock") == 0);
   assert(seen_port == 8443);
   assert(strcmp(seen_path, "/model/demo.model/converse") == 0);
   assert(strstr(seen_auth, "Authorization: AWS4-HMAC-SHA256 ") != NULL);
   assert(strstr(seen_auth, "X-Amz-Security-Token: TOKEN") != NULL);
   assert(status == 200 && strstr(out, "ok") != NULL);
   assert(kb_bedrock_dispatch_https(&t, &ir, 1, "AKID", "SECRET", NULL,
                                    "20260101T000000Z", "20260101", "CA", NULL,
                                    out, sizeof(out), &status) == 0);
   assert(strcmp(seen_path, "/model/demo.model/converse-stream") == 0);
   return 0;
}
