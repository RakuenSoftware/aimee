/* The actual hook exporter must not silently clip or repair external sources. */
#include "../cli_session_start.c"
#include <assert.h>
#include <fcntl.h>
#include "platform_test_util.h"

static int posts;
cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body, const char *bearer, int timeout, int *status)
{
   (void)endpoint;
   (void)bearer;
   (void)timeout;
   assert(!strcmp(method, "POST") && !strcmp(path, "/v1/sessions/record_transcript"));
   cJSON *value = cJSON_Parse(body);
   assert(value && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value, "messages")) == 2);
   cJSON_Delete(value);
   posts++;
   *status = 200;
   return cJSON_CreateObject();
}
static void export_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(text, f);
   fclose(f);
   ss_record_transcript("fixture", "bearer", "session", path);
}
int main(void)
{
   char path[4096];
   snprintf(path, sizeof path, "%s/aimee-transcript-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(path, sizeof path, "aimee-transcript-");
   assert(fd >= 0);
   close(fd);
   export_text(path, "{\"type\":\"user\"}\n{\"type\":\"assistant\"}\n");
   assert(posts == 1);
   export_text(path, "{\"type\":\"user\"}\ninvalid\n{\"type\":\"assistant\"}\n");
   assert(posts == 1);
   export_text(path, "{\"type\":\"user\"} trailing\n{\"type\":\"assistant\"}\n");
   assert(posts == 1);
   export_text(path, "{\"type\":\"user\"}\n{\"type\":");
   assert(posts == 1);
   fd = open(path, O_WRONLY | O_TRUNC);
   assert(fd >= 0);
   assert(ftruncate(fd, SS_TRANSCRIPT_MAX + 1) == 0);
   close(fd);
   ss_record_transcript("fixture", "bearer", "session", path);
   assert(posts == 1);
   unlink(path);
   puts("hook export: complete JSONL only; malformed, trailing, partial and oversized sources "
        "refused");
   return 0;
}
