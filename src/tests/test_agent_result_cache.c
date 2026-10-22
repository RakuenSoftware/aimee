/* Exercise the actual result-cache owner with deterministic storage and source
 * receipts. Native IR admission, codec, ownership and liveness are real. */
#include "agent_result_cache.h"
#include "agent_config.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <stdio.h>

static const char *principal = "owner-a", *session = "";
static char *stored;
static int gets, writes, invalid, checks, invalidate_at;
const char *agent_get_request_vault_principal(void)
{
   return principal;
}
const char *agent_get_request_session(void)
{
   return session;
}
int wire_fence_revalidate_sources(void)
{
   checks++;
   return invalid || (invalidate_at && checks >= invalidate_at);
}
char *db1_agent_cache_get(const char *role, const char *key)
{
   assert(!strcmp(role, "summarize") && strlen(key) == 64);
   gets++;
   return stored ? strdup(stored) : NULL;
}
void db1_agent_cache_put(const char *role, const char *key, const char *wire)
{
   assert(!strcmp(role, "summarize") && strlen(key) == 64);
   writes++;
   free(stored);
   stored = strdup(wire);
   assert(stored);
}
static int sink(const aimee_session_event_t *event, void *context)
{
   (void)event;
   (void)context;
   return 0;
}
int main(void)
{
   agent_t agent = {0};
   strcpy(agent.name, "worker");
   strcpy(agent.model, "model-a");
   char key[65], other[65];
   assert(agent_result_cache_open("summarize", &agent, "request", NULL, NULL, key));
   principal = "owner-b";
   assert(agent_result_cache_open("summarize", &agent, "request", NULL, NULL, other));
   assert(strcmp(key, other));
   principal = "";
   assert(!agent_result_cache_open("summarize", &agent, "request", NULL, NULL, other));
   principal = "owner-a";
   session = "user-turn";
   assert(!agent_result_cache_open("summarize", &agent, "request", NULL, NULL, other));
   session = "";
   assert(!agent_result_cache_open("summarize", &agent, "request", "credential", NULL, other));
   assert(!agent_result_cache_open("summarize", &agent, "request", NULL, "account", other));
   strcpy(agent.fallback_model, "fallback");
   assert(!agent_result_cache_open("summarize", &agent, "request", NULL, NULL, other));
   agent.fallback_model[0] = 0;
   aimee_ir_session_set_sink(sink, NULL);
   assert(!agent_result_cache_open("summarize", &agent, "request", NULL, NULL, other));
   aimee_ir_session_set_sink(NULL, NULL);
   assert(!gets && !writes);

   agent_result_t original = {0}, reused = {0};
   /* This is test input to the typed cache, not a native adapter fallback. */
   assert(!agent_result_from_host_text(&original, "A useful complete summary."));
   original.success = 1;
   original.canonical_response.model = strdup("reported-model");
   original.canonical_response.usage_in = 40;
   original.canonical_response.usage_cache_read = 21;
   original.canonical_response.stop_reason = AIMEE_STOP_END_TURN;
   original.canonical_response.raw = cJSON_Parse("{\"opaque\":true}");
   agent_result_cache_write("summarize", key, &original);
   assert(writes == 1 && stored);
   assert(agent_result_cache_read("summarize", key, &reused));
   assert(reused.success && reused.cache_hit && !reused.prompt_tokens && !reused.completion_tokens);
   assert(!strcmp(reused.response, original.response));
   assert(reused.canonical_response.content != original.canonical_response.content);
   assert(reused.canonical_response.usage_cache_read == 21);
   assert(cJSON_Compare(reused.canonical_response.raw, original.canonical_response.raw, 1));
   agent_result_cache_write("summarize", key, &reused);
   assert(writes == 1);
   agent_result_clear_response(&reused);
   invalid = 1;
   int before = gets;
   assert(!agent_result_cache_read("summarize", key, &reused) && gets == before);
   agent_result_cache_write("summarize", key, &original);
   assert(writes == 1);
   invalid = 0;
   checks = 0;
   invalidate_at = 2;
   assert(!agent_result_cache_read("summarize", key, &reused));
   assert(!reused.canonical_response.content && !reused.response);
   invalidate_at = 0;
   free(stored);
   stored = strdup("legacy flat response");
   assert(!agent_result_cache_read("summarize", key, &reused));
   assert(!reused.canonical_response.content);
   agent_result_clear_response(&original);
   free(stored);
   puts("agent-result-cache: canonical reuse and ownership gates passed");
   return 0;
}
