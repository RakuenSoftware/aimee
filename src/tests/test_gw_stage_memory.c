/* test_gw_stage_memory.c: unit tests for the ONE shared memory-injection stage
 * (universal-gateway P3, gw_stage_memory.c). The point of P3 is consolidation
 * WITHOUT changing rendered bytes, so these are byte-identity tests across the
 * three render targets:
 *   - OPENAI_SYSTEM_PROMPT (legacy text handlers): the RAW envelope, byte-for-
 *     byte what ingress_preinject_build(query, 0) returned inline before P3 —
 *     crucially NO trailing "\n\n".
 *   - OPENAI_INSTRUCTIONS (/v1/responses): env + "\n\n" + prior, via apply().
 *   - ANTHROPIC_MESSAGES: dispatch + the parity gate (the Anthropic applier
 *     itself, messages_apply_preinject, lives in anthropic_http.c and is covered
 *     by the anthropic tests; here it is stubbed to assert dispatch/gating).
 *
 * The kb client / config are stubbed (as in test_ingress_preinject.c) so
 * ingress_preinject_build produces a deterministic envelope; platform_random is
 * FIXED here so two build() calls render identically and can be compared. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gw_stage_memory.h"
#include "ingress_preinject.h"
#include "cJSON.h"
#include "config.h"
#include "kb_client.h"

/* When set, the recall stubs return nothing so ingress_preinject_build → NULL
 * (the "pre-injection off / recall empty" path). */
static int g_no_recall = 0;
static int g_test_placement = 0; /* drives ingress_cache_placement_enabled in config_load stub */

/* --- stubs: make ingress_preinject_build deterministic without the kb graph --- */
char *kb_client_memory_context_block(const char *query, const char *block_type, int limit)
{
   (void)query;
   (void)block_type;
   (void)limit;
   return NULL;
}
char *kb_client_memory_facts(const char *query)
{
   (void)query;
   return NULL;
}
int kb_client_memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   (void)query;
   (void)limit;
   if (g_no_recall || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(out[0]) * (size_t)max);
   out[0].memory.id = 101;
   snprintf(out[0].memory.tier, sizeof(out[0].memory.tier), "L2");
   snprintf(out[0].memory.kind, sizeof(out[0].memory.kind), "fact");
   snprintf(out[0].memory.key, sizeof(out[0].memory.key), "deploy path");
   snprintf(out[0].memory.headline, sizeof(out[0].memory.headline), "Use the deploy matrix.");
   out[0].parts.total = 0.88;
   return 1;
}
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   if (g_no_recall || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(out[0]) * (size_t)max);
   snprintf(out[0].file_path, sizeof(out[0].file_path), "src/server/ingress_preinject.c");
   snprintf(out[0].snippet, sizeof(out[0].snippet), "builder emits a bounded context envelope");
   return 1;
}
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->ingress_preinject_enabled = 1;
      cfg->ingress_preinject_assembly_budget = 1200;
      cfg->ingress_cache_placement_enabled = g_test_placement;
   }
   return 0;
}
const char *config_default_dir(void)
{
   return "/tmp/aimee-test";
}
int kb_client_evidence_emit_retrieval_event(const char *turn_id, const char *role,
                                            const char *query_fingerprint, const int64_t *ids,
                                            int n_ids)
{
   (void)turn_id;
   (void)role;
   (void)query_fingerprint;
   (void)ids;
   (void)n_ids;
   return 0;
}
int kb_client_evidence_merge_retrieval_event(const char *turn_id, const char *role,
                                             const char *query_fingerprint,
                                             const char *const *types, const char *const *refs,
                                             const char *const *versions, int n)
{
   (void)turn_id;
   (void)role;
   (void)query_fingerprint;
   (void)types;
   (void)refs;
   (void)versions;
   (void)n;
   return 0;
}
/* FIXED (not varying): the envelope must render identically across two build()
 * calls so the byte-identity comparisons below are meaningful. */
int platform_random_bytes(void *buf, size_t len)
{
   memset(buf, 0x5a, len);
   return 0;
}

static const char *instr_of(cJSON *raw)
{
   cJSON *i = cJSON_GetObjectItemCaseSensitive(raw, "instructions");
   return (i && cJSON_IsString(i)) ? i->valuestring : NULL;
}

/* OPENAI_SYSTEM_PROMPT: gw_memory_system_prompt(q) == ingress_preinject_build(q,0)
 * byte-for-byte (the raw env, no trailing "\n\n"). */
static void test_system_prompt_raw_env(void)
{
   char *sys = gw_memory_system_prompt("deploy matrix");
   char *direct = ingress_preinject_build("deploy matrix", 0);
   assert(sys != NULL && direct != NULL);
   assert(strcmp(sys, direct) == 0);
   /* The legacy contract: NOT apply()-merged, so no trailing blank line. */
   size_t n = strlen(sys);
   assert(!(n >= 2 && sys[n - 1] == '\n' && sys[n - 2] == '\n'));
   free(sys);
   free(direct);
   printf("system_prompt_raw_env OK\n");
}

/* OPENAI_INSTRUCTIONS with a prior system: env + "\n\n" + prior, identical to
 * ingress_preinject_apply(prior, env). */
static void test_instructions_merge_with_prior(void)
{
   cJSON *messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"deploy matrix\"}]");
   char *q = ingress_preinject_query_from_messages(messages);
   char *env = ingress_preinject_build(q, 0);
   char *expected = ingress_preinject_apply("PRIOR SYS", env);
   assert(env && expected);

   cJSON *raw = cJSON_CreateObject();
   cJSON_AddStringToObject(raw, "instructions", "PRIOR SYS");
   gw_request_t r = {.raw = raw,
                     .serving_api = GW_API_OPENAI,
                     .mem_target = GW_MEM_OPENAI_INSTRUCTIONS,
                     .parity = 1};
   int rc = gw_stage_memory(&r, messages);
   assert(rc == 1);
   assert(strcmp(instr_of(raw), expected) == 0);

   free(q);
   free(env);
   free(expected);
   cJSON_Delete(raw);
   cJSON_Delete(messages);
   printf("instructions_merge_with_prior OK\n");
}

/* OPENAI_INSTRUCTIONS with NO prior system: env + "\n\n". Confirms the two
 * OpenAI targets differ by exactly the trailing "\n\n". */
static void test_instructions_no_prior(void)
{
   cJSON *messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"deploy matrix\"}]");
   char *raw_env = gw_memory_system_prompt("deploy matrix"); /* the legacy raw env */
   assert(raw_env);

   cJSON *raw = cJSON_CreateObject();
   cJSON_AddStringToObject(raw, "instructions", "");
   gw_request_t r = {.raw = raw,
                     .serving_api = GW_API_OPENAI,
                     .mem_target = GW_MEM_OPENAI_INSTRUCTIONS,
                     .parity = 1};
   int rc = gw_stage_memory(&r, messages);
   assert(rc == 1);

   char *expect = malloc(strlen(raw_env) + 3);
   sprintf(expect, "%s\n\n", raw_env);
   assert(strcmp(instr_of(raw), expect) == 0);

   free(expect);
   free(raw_env);
   cJSON_Delete(raw);
   cJSON_Delete(messages);
   printf("instructions_no_prior OK\n");
}

/* ANTHROPIC_MESSAGES: parity-gated — the envelope is appended as a trailing
 * `system` text block only when !parity (passthrough must not perturb the cached
 * prefix). The query is derived from raw.messages (ud unused). */
static void test_anthropic_parity_gate(void)
{
   /* parity ON → passthrough, system array stays empty. */
   cJSON *raw = cJSON_Parse(
       "{\"messages\":[{\"role\":\"user\",\"content\":\"deploy matrix\"}],\"system\":[]}");
   gw_request_t r = {.raw = raw,
                     .serving_api = GW_API_ANTHROPIC,
                     .mem_target = GW_MEM_ANTHROPIC_MESSAGES,
                     .parity = 1};
   assert(gw_stage_memory(&r, NULL) == 0);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(raw, "system")) == 0);

   /* parity OFF → one <aimee-context> text block appended to system. */
   r.parity = 0;
   assert(gw_stage_memory(&r, NULL) == 0);
   cJSON *sys = cJSON_GetObjectItemCaseSensitive(raw, "system");
   assert(cJSON_GetArraySize(sys) == 1);
   cJSON *blk = cJSON_GetArrayItem(sys, 0);
   const cJSON *type = cJSON_GetObjectItemCaseSensitive(blk, "type");
   const cJSON *text = cJSON_GetObjectItemCaseSensitive(blk, "text");
   assert(type && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0);
   assert(text && cJSON_IsString(text) && strstr(text->valuestring, "aimee-context") != NULL);

   cJSON_Delete(raw);
   printf("anthropic_parity_gate OK\n");
}

/* P5 (§2.3): the opt-in — with allow_anthropic_inject set, the envelope IS
 * injected even under parity (the Anthropic-native passthrough), appended as a
 * trailing system text block so the cached prefix survives. */
static void test_anthropic_parity_opt_in_inject(void)
{
   cJSON *raw = cJSON_Parse(
       "{\"messages\":[{\"role\":\"user\",\"content\":\"deploy matrix\"}],\"system\":[]}");
   gw_request_t r = {.raw = raw,
                     .serving_api = GW_API_ANTHROPIC,
                     .mem_target = GW_MEM_ANTHROPIC_MESSAGES,
                     .parity = 1,
                     .allow_anthropic_inject = 1};
   assert(gw_stage_memory(&r, NULL) == 0);
   cJSON *sys = cJSON_GetObjectItemCaseSensitive(raw, "system");
   assert(cJSON_GetArraySize(sys) == 1); /* injected despite parity */
   cJSON *blk = cJSON_GetArrayItem(sys, 0);
   const cJSON *text = cJSON_GetObjectItemCaseSensitive(blk, "text");
   assert(text && cJSON_IsString(text) && strstr(text->valuestring, "aimee-context") != NULL);

   cJSON_Delete(raw);
   printf("anthropic_parity_opt_in_inject OK\n");
}

/* Pre-injection off / recall empty: every target is a byte-identical no-op. */
static void test_disabled_noop(void)
{
   g_no_recall = 1;

   assert(gw_memory_system_prompt("deploy matrix") == NULL);

   cJSON *messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"deploy matrix\"}]");
   cJSON *raw = cJSON_CreateObject();
   cJSON_AddStringToObject(raw, "instructions", "KEEP");
   gw_request_t r = {.raw = raw,
                     .serving_api = GW_API_OPENAI,
                     .mem_target = GW_MEM_OPENAI_INSTRUCTIONS,
                     .parity = 1};
   assert(gw_stage_memory(&r, messages) == 0);
   assert(strcmp(instr_of(raw), "KEEP") == 0); /* untouched */

   cJSON_Delete(raw);
   cJSON_Delete(messages);
   g_no_recall = 0;
   printf("disabled_noop OK\n");
}

/* Cache-prefix placement (§2): with ingress_cache_placement_enabled on, the
 * OPENAI_INSTRUCTIONS arm APPENDS the envelope after the stable prior prefix
 * (prior + "\n\n" + env) instead of prepending — so the provider prefix cache is
 * not invalidated by the per-turn envelope. */
static void test_instructions_placement_appends(void)
{
   cJSON *messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"deploy matrix\"}]");
   char *q = ingress_preinject_query_from_messages(messages);
   char *env = ingress_preinject_build(q, 0);
   char *prepended = ingress_preinject_apply("PRIOR SYS", env); /* env first (off) */
   char *appended = ingress_preinject_append("PRIOR SYS", env); /* env last (on)  */
   assert(env && prepended && appended);
   assert(strcmp(prepended, appended) != 0); /* order differs */

   g_test_placement = 1;
   cJSON *raw = cJSON_CreateObject();
   cJSON_AddStringToObject(raw, "instructions", "PRIOR SYS");
   gw_request_t r = {.raw = raw,
                     .serving_api = GW_API_OPENAI,
                     .mem_target = GW_MEM_OPENAI_INSTRUCTIONS,
                     .parity = 1};
   int rc = gw_stage_memory(&r, messages);
   assert(rc == 1);
   /* stable prefix stays at the front; envelope is the volatile suffix */
   assert(strcmp(instr_of(raw), appended) == 0);
   assert(strncmp(instr_of(raw), "PRIOR SYS", 9) == 0);
   g_test_placement = 0;

   free(q);
   free(env);
   free(prepended);
   free(appended);
   cJSON_Delete(raw);
   cJSON_Delete(messages);
   printf("instructions_placement_appends OK\n");
}

int main(void)
{
   printf("test_gw_stage_memory:\n");
   test_system_prompt_raw_env();
   test_instructions_merge_with_prior();
   test_instructions_no_prior();
   test_instructions_placement_appends();
   test_anthropic_parity_gate();
   test_anthropic_parity_opt_in_inject();
   test_disabled_noop();
   printf("all gw_stage_memory tests passed\n");
   return 0;
}
