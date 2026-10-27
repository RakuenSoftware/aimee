#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../gateway/platform_ntfy.h"
#include "agent_exec.h"
#include "runtime_secret.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static platform_adapter_t *registered;
static const void *expected;
static size_t expected_length;
static const char *expected_type;
static int status = 200, calls, timeout_ms;

int gateway_platform_register(platform_adapter_t *adapter)
{
   registered = adapter;
   return 0;
}
int agent_http_post_content_type_bytes(const char *url, const char *auth, const char *type,
                                       const void *body, size_t length, char **response,
                                       size_t *response_length, int timeout, const char *extra)
{
   assert(!strcmp(url, "https://ntfy.example/topic"));
   assert(auth && !strcmp(auth, "Authorization: Bearer fixture-token"));
   assert(!strcmp(type, expected_type) && timeout == timeout_ms && !extra);
   assert(length == expected_length && !memcmp(body, expected, length));
   assert(response && response_length);
   *response = strdup("{}");
   *response_length = 2;
   assert(*response);
   calls++;
   return status;
}
int main(void)
{
   assert(!setenv("AIMEE_GATEWAY_NTFY_BASE_URL", "https://ntfy.example", 1));
   assert(!runtime_secret_store("AIMEE_GATEWAY_NTFY_TOKEN", "fixture-token"));
   platform_adapter_t *adapter = ntfy_adapter_get();
   assert(adapter == registered);
   delivery_target_t target = {0};
   snprintf(target.chat_id, sizeof target.chat_id, "%s", "topic");
   expected = "message \"quoted\"\nnext";
   expected_length = strlen(expected);
   expected_type = "text/plain";
   timeout_ms = 30000;
   int statuses[] = {200, 201, 204, 299, -1, 0, 199, 300, 401, 500};
   for (size_t i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
   {
      status = statuses[i];
      assert((adapter->send_text(adapter, &target, expected) == 0) ==
             (status >= 200 && status < 300));
   }
   const char *temporary = getenv("TMPDIR");
   if (!temporary || !temporary[0])
      temporary = "/tmp";
   char path[4096];
   int n = snprintf(path, sizeof path, "%s/ntfy-message-XXXXXX", temporary);
   assert(n > 0 && (size_t)n < sizeof path);
   int fd = mkstemp(path);
   assert(fd >= 0);
   static const unsigned char binary[] = {'R', 0, 'I', 'F', 0xff, 0, 'X'};
   assert(write(fd, binary, sizeof binary) == sizeof binary && !close(fd));
   expected = binary;
   expected_length = sizeof binary;
   expected_type = "audio/wav";
   timeout_ms = 60000;
   for (size_t i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
   {
      status = statuses[i];
      assert((adapter->send_attachment(adapter, &target, path, expected_type) == 0) ==
             (status >= 200 && status < 300));
   }
   expected_type = "application/octet-stream";
   status = 200;
   assert(!adapter->send_attachment(adapter, &target, path, NULL));
   assert(!unlink(path));
   int sent = calls;
   assert(adapter->send_attachment(adapter, &target, path, NULL) == -1);
   assert(calls == sent);
   adapter->shutdown(adapter);
   runtime_secret_clear();
   puts("ntfy: complete text/binary messages, HTTP status and owned reply cleanup pass");
   return 0;
}
