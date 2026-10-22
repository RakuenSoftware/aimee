#ifndef AIMEE_NATIVE_BETA_PATH_FIXTURE_H
#define AIMEE_NATIVE_BETA_PATH_FIXTURE_H
#include "aimee_sha256.h"
static int native_beta_captures;
int anthropic_http_capture_native_request_path(const char *path)
{
   if (strstr(path, "?beta=true"))
   {
      assert(!strcmp(path, "/v1/messages?beta=true"));
      native_beta_captures++;
   }
   return 0;
}
static void request(const server_native_runtime_t *, const char *, const char *, char[1024]);
static void test_unowned_beta_path(const server_native_runtime_t *runtime)
{
   char response[1024];
   int previous = native_beta_captures;
   request(runtime, "/v1/messages?beta=true", server_native_runtime_token(runtime), response);
   assert(strstr(response, "404") && native_beta_captures == previous);
}
static void test_beta_contract_binding(const agent_t *selected, const request_context_t *owner)
{
   char lineage[65], legacy[65], current[65], bytes[4096];
   memset(lineage, 'a', 64);
   lineage[64] = 0;
   const char *parts[] = {owner->vault_principal,
                          selected->registration,
                          selected->name,
                          selected->provider,
                          selected->model,
                          selected->endpoint,
                          "selected-vendor-account",
                          lineage};
   size_t used = 0;
   for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++)
      used +=
          (size_t)snprintf(bytes + used, sizeof bytes - used, "%zu:%s", strlen(parts[i]), parts[i]);
   assert(!aimee_sha256_hex(bytes, used, legacy));
   const char contract[] = "native-anthropic-beta-query-v1";
   used +=
       (size_t)snprintf(bytes + used, sizeof bytes - used, "%zu:%s", strlen(contract), contract);
   assert(!aimee_sha256_hex(bytes, used, current));
   owner_tools(1);
   server_session_capture_t *capture =
       server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
   assert(capture && !server_session_capture_bind(capture));
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "host system"};
   aimee_request_t profile = {.frontend = AIMEE_WIRE_ANTHROPIC,
                              .system = &system,
                              .n_system = 1,
                              .model = "selected-model",
                              .mutated = 1};
   for (int kind = 0; kind < 3; kind++)
   {
      profile.frontend = kind == 2 ? AIMEE_WIRE_RESPONSES : AIMEE_WIRE_ANTHROPIC;
      server_native_runtime_t *runtime =
          server_native_runtime_open_request(selected, owner, "session-a", &profile);
      assert(runtime);
      assert(!strcmp(server_native_runtime_binding(runtime), kind == 2 ? legacy : current));
      server_native_runtime_close(runtime);
   }
   assert(server_session_capture_bind(NULL) == capture);
   server_session_capture_close(capture);
}
#endif
