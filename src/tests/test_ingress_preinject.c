/* test_ingress_preinject.c: unit tests for the P1 pre-injection pure helpers
 * (confidence tiering, envelope formatting, query extraction, apply/merge).
 * The kb-backed builder (ingress_preinject_build) is not exercised here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ingress_preinject.h"
#include "cJSON.h"
#include "config.h"
#include "kb_client.h"
#include "request_context.h"

static const char *g_context_mode = "observe";
static int g_context_calls = 0;
static int g_facts_enabled = 0;
static int g_facts_calls = 0;
static kb_client_result_status_t g_context_result = KB_CLIENT_RESULT_OK;

/* The kb-backed builder (ingress_preinject_build) is out of scope here; these
 * stubs satisfy the linker so the test links only the pure helpers without
 * dragging in the kb client / config-load object graph. */
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
   g_facts_calls++;
   return strdup("- global preference: never substitute for project evidence\n");
}

/* Typed-facts gate stub: off, so the builder's facts path stays inert here
 * (kb_client_memory_facts above already returns NULL). ingress_preinject.c gained
 * this call with the typed_facts feature; the test link needs the symbol. */
int kb_client_typed_facts_enabled(void)
{
   return g_facts_enabled;
}
int ingress_preinject_resolve_active_scope(char *workspace, size_t workspace_len, char *project,
                                           size_t project_len)
{
   snprintf(workspace, workspace_len, "active-workspace");
   snprintf(project, project_len, "active-project");
   return 0;
}
void kb_client_memory_scope_context_set(const char *workspace, const char *project, int include_all)
{
   (void)workspace;
   (void)project;
   assert(include_all == 0);
}
void kb_client_memory_scope_context_clear(void)
{
}
char *kb_client_code_context(const char *query, const char *symbol, const char *project,
                             int *status_out)
{
   assert(query && query[0]);
   (void)symbol;
   assert(project && strcmp(project, "active-project") == 0);
   g_context_calls++;
   if (g_context_result == KB_CLIENT_RESULT_UNAVAILABLE)
   {
      if (status_out)
         *status_out = 503;
      return NULL;
   }
   if (status_out)
      *status_out = 200;
   return strdup("{\"status\":\"ok\",\"project\":\"active-project\",\"generation\":7,"
                 "\"freshness\":\"current\",\"resolved\":true,"
                 "\"max_results\":4,\"max_tokens\":1200,\"item_count\":1,"
                 "\"answerability\":{\"decision\":\"answerable\"},\"results\":[{"
                 "\"project\":\"active-project\",\"file_path\":\"src/local.c\","
                 "\"generation\":7,\"freshness\":\"current\",\"confidence\":0.95,"
                 "\"accepted\":true,\"provenance\":[\"code\"],"
                 "\"span\":{\"kind\":\"line\",\"line_start\":12,\"line_end\":12},"
                 "\"snippet\":\"int local_answer(void);\"}],\"why\":[]}");
}
kb_client_result_status_t kb_client_last_result_status(void)
{
   return g_context_result;
}
/* When set, memory recall returns nothing -- the shape of both a quiet turn and
 * an outage, which is exactly the pair the degraded-recall counter separates. */
static int g_memory_returns_none = 0;

int kb_client_memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   (void)query;
   (void)limit;
   if (g_memory_returns_none)
      return 0;
   if (!out || max <= 0)
      return 0;
   memset(out, 0, sizeof(out[0]) * (size_t)max);
   out[0].memory.id = 101;
   snprintf(out[0].memory.tier, sizeof(out[0].memory.tier), "L2");
   snprintf(out[0].memory.kind, sizeof(out[0].memory.kind), "fact");
   snprintf(out[0].memory.key, sizeof(out[0].memory.key), "deploy path");
   snprintf(out[0].memory.headline, sizeof(out[0].memory.headline), "Use the deploy matrix.");
   out[0].parts.total = 0.88;
   if (max == 1)
      return 1;
   out[1].memory.id = 102;
   snprintf(out[1].memory.tier, sizeof(out[1].memory.tier), "L2");
   snprintf(out[1].memory.kind, sizeof(out[1].memory.kind), "policy");
   snprintf(out[1].memory.key, sizeof(out[1].memory.key), "fallback");
   snprintf(out[1].memory.content, sizeof(out[1].memory.content), "Fallback preview from content.");
   out[1].parts.total = 0.44;
   return 2;
}
/* Drives the compression lever in the build test below (config_load stub). */
static int g_test_compress = 0;

int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   if (!out || max <= 0)
      return 0;
   memset(out, 0, sizeof(out[0]) * (size_t)max);
   snprintf(out[0].file_path, sizeof(out[0].file_path), "src/server/ingress_preinject.c");
   if (g_test_compress)
   {
      /* A snippet over the 80-char fold threshold + a known matched line, so the
       * fold path is exercised. Only when compressing, so the P0 byte-equivalence
       * golden below (default-off) stays the original short-snippet fixture. */
      snprintf(out[0].snippet, sizeof(out[0].snippet),
               "the builder emits a bounded context envelope from a typed entry list and renders "
               "the recommended code block before exploring");
      out[0].line = 42;
   }
   else
   {
      snprintf(out[0].snippet, sizeof(out[0].snippet), "builder emits a bounded context envelope");
   }
   return 1;
}
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->ingress_preinject_enabled = 1;
      cfg->ingress_preinject_assembly_budget = 1200;
      cfg->ingress_compress_enabled = g_test_compress;
      /* -1 = unspecified: memset-0 would read as user-disabled and gate the memory module. */
      cfg->module_memory = cfg->module_governance = -1;
      cfg->module_delegates = cfg->module_workflows = -1;
   }
   return 0;
}

/* Accessor stubs: the production seam moved from config_load to per-field
 * accessors. Values mirror exactly what the stub above writes into the struct —
 * preinject on, budget 1200, compress tracking g_test_compress, and the two
 * fields the stub leaves zeroed — so no assertion changes meaning. */
int config_ingress_preinject_enabled(void)
{
   return 1;
}

const char *config_code_context_mode(void)
{
   return g_context_mode;
}

int config_ingress_preinject_assembly_budget(void)
{
   return 1200;
}

int config_ingress_compress_enabled(void)
{
   return g_test_compress;
}

int config_ingress_compress_min_chars(void)
{
   return 0;
}

int config_ingress_cache_placement_enabled(void)
{
   return 0;
}

int config_kb_evidence_emit_enabled(void)
{
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
int kb_client_evidence_emit_retrieval_event_ex(const char *turn_id, const char *role,
                                               const char *query_fingerprint, const int64_t *ids,
                                               int n_ids, char *event_id_out, size_t event_id_len)
{
   (void)turn_id;
   (void)role;
   (void)query_fingerprint;
   (void)ids;
   (void)n_ids;
   if (event_id_out && event_id_len > 0)
      event_id_out[0] = '\0';
   return 0;
}
void retrieval_outcome_bridge_note(const char *surface, const char *event_id, const int64_t *ids,
                                   const char *const *snippets, int n)
{
   (void)surface;
   (void)event_id;
   (void)ids;
   (void)snippets;
   (void)n;
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
/* Stub: deterministic but varying-per-call, so the mint-uniqueness assertion
 * holds without linking the platform layer into this pure unit test. */
int platform_random_bytes(void *buf, size_t len)
{
   static unsigned char ctr = 0;
   unsigned char *p = (unsigned char *)buf;
   for (size_t i = 0; i < len; i++)
      p[i] = (unsigned char)(ctr + i);
   ctr++;
   return 0;
}

static int test_confidence_provider(double score, const char **confidence)
{
   *confidence = score >= 0.66 ? "high" : score >= 0.33 ? "medium" : "low";
   return 0;
}

static int failing_confidence_provider(double score, const char **confidence)
{
   (void)score;
   (void)confidence;
   return -1;
}

static int invalid_confidence_provider(double score, const char **confidence)
{
   (void)score;
   *confidence = "unknown";
   return 0;
}

static void test_confidence_tiers(void)
{
   const char *confidence = "stale";
   ingress_preinject_register_confidence_provider(NULL);
   assert(ingress_preinject_confidence(0.9, &confidence) == -1 && confidence == NULL);
   ingress_preinject_register_confidence_provider(failing_confidence_provider);
   assert(ingress_preinject_confidence(0.9, &confidence) == -1 && confidence == NULL);
   ingress_preinject_register_confidence_provider(invalid_confidence_provider);
   assert(ingress_preinject_confidence(0.9, &confidence) == -1 && confidence == NULL);

   ingress_preinject_register_confidence_provider(test_confidence_provider);
   assert(ingress_preinject_confidence(0.9, &confidence) == 0 && strcmp(confidence, "high") == 0);
   assert(ingress_preinject_confidence(0.66, &confidence) == 0 && strcmp(confidence, "high") == 0);
   assert(ingress_preinject_confidence(0.5, &confidence) == 0 && strcmp(confidence, "medium") == 0);
   assert(ingress_preinject_confidence(0.33, &confidence) == 0 &&
          strcmp(confidence, "medium") == 0);
   assert(ingress_preinject_confidence(0.1, &confidence) == 0 && strcmp(confidence, "low") == 0);
   assert(ingress_preinject_confidence(0.0, &confidence) == 0 && strcmp(confidence, "low") == 0);
   printf("confidence_tiers OK\n");
}

static void test_format_envelope(void)
{
   /* NULL / blank block -> no envelope. */
   assert(ingress_preinject_format_envelope(NULL, "high") == NULL);
   assert(ingress_preinject_format_envelope("   \n\t ", "high") == NULL);

   char *e = ingress_preinject_format_envelope("recommended:\n  - src/a.c::f", "high");
   assert(e != NULL);
   assert(strstr(e, "<aimee-context confidence=\"high\">") == e); /* opens at start */
   assert(strstr(e, "src/a.c::f") != NULL);
   /* The per-turn envelope carries RETRIEVAL ONLY. The standing guidance moved to
    * a session-start injection on the IR (ir_stage_memory): it used to ride in
    * here, which meant aimee only told an agent to use aimee's tools once aimee
    * had already retrieved something -- so on an unindexed repo the agent was told
    * nothing. Guidance is not per-turn and not conditional on recall. */
   assert(strstr(e, "explore-with: ") == NULL);
   assert(strstr(e, "fix-scope: ") == NULL);
   assert(strstr(e, "</aimee-context>") != NULL);
   free(e);

   /* Confidence must come from the supervised memory stage. */
   assert(ingress_preinject_format_envelope("x", NULL) == NULL);
   assert(ingress_preinject_format_envelope("x", "unknown") == NULL);
   printf("format_envelope OK\n");
}

static void test_format_task_context_strict_contract(void)
{
   const char *ok = "{\"status\":\"ok\",\"project\":\"active-project\",\"generation\":7,"
                    "\"freshness\":\"current\",\"resolved\":true,"
                    "\"max_results\":4,\"max_tokens\":1200,\"item_count\":2,"
                    "\"answerability\":{\"decision\":\"answerable\"},\"results\":[{"
                    "\"project\":\"active-project\",\"file_path\":\"src/local.c\","
                    "\"generation\":7,\"freshness\":\"current\",\"confidence\":0.95,"
                    "\"accepted\":true,\"provenance\":[\"code\",\"graph\"],"
                    "\"span\":{\"kind\":\"line\",\"line_start\":12,\"line_end\":12},"
                    "\"snippet\":\"int local_answer(void);\"}],\"why\":[{"
                    "\"memory_id\":9,\"content\":\"Use the local resolver.\","
                    "\"scope\":\"project\",\"provenance\":\"memory\","
                    "\"confidence\":0.9,\"anchor\":{\"project\":\"active-project\","
                    "\"file_path\":\"src/local.c\",\"generation\":7,"
                    "\"freshness\":\"current\"}}]}";
   int count = 0;
   double confidence = 0.0;
   char *packet = ingress_preinject_format_task_context(ok, "active-project", &count, &confidence);
   assert(packet && count == 2 && confidence == 0.95);
   assert(strstr(packet, "task-conditioned code; project=active-project; generation=7") != NULL);
   assert(strstr(packet, "src/local.c:12") != NULL);
   assert(strstr(packet, "provenance=code,graph") != NULL);
   assert(strstr(packet, "memory[project") != NULL);
   free(packet);

   const char *no_answer =
       "{\"status\":\"no_answer\",\"project\":\"active-project\",\"generation\":7,"
       "\"freshness\":\"current\",\"resolved\":true,"
       "\"answerability\":{\"decision\":\"no_answer\"},\"results\":[],\"why\":[]}";
   assert(ingress_preinject_format_task_context(no_answer, "active-project", NULL, NULL) == NULL);

   char *wrong = strdup(ok);
   char *project = strstr(wrong, "active-project");
   memcpy(project, "foreign-projec", 14);
   assert(ingress_preinject_format_task_context(wrong, "active-project", NULL, NULL) == NULL);
   free(wrong);

   char *high_code_confidence = strdup(ok);
   char *code_confidence = strstr(high_code_confidence, "0.95");
   assert(code_confidence != NULL);
   code_confidence[0] = '1';
   assert(ingress_preinject_format_task_context(high_code_confidence, "active-project", NULL,
                                                NULL) == NULL);
   free(high_code_confidence);

   char *high_memory_confidence = strdup(ok);
   char *memory_confidence = strstr(high_memory_confidence, "0.9,\"anchor\"");
   assert(memory_confidence != NULL);
   memory_confidence[0] = '1';
   assert(ingress_preinject_format_task_context(high_memory_confidence, "active-project", NULL,
                                                NULL) == NULL);
   free(high_memory_confidence);
   printf("format_task_context_strict_contract OK\n");
}

static void test_task_context_mode_and_first_turn_gate(void)
{
   ingress_preinject_task_state_reset();
   ingress_preinject_set_session_id("session-task-1");
   g_context_calls = 0;
   g_facts_enabled = 1;
   g_facts_calls = 0;
   g_context_mode = "on";
   g_context_result = KB_CLIENT_RESULT_OK;

   char *first = ingress_preinject_build("fix local resolver", 0);
   assert(first && strstr(first, "recommended (task-conditioned code") != NULL);
   assert(strstr(first, "src/local.c:12") != NULL);
   assert(strstr(first, "memory previews") == NULL);
   assert(strstr(first, "global preference") == NULL);
   assert(g_facts_calls == 0);
   free(first);
   assert(g_context_calls == 1);

   /* Related vocabulary remains the same task: no repeated packet and no
    * fallback to the legacy/global preview while the strict mode is on. */
   char *followup = ingress_preinject_build("please fix the local resolver", 0);
   assert(followup == NULL);
   assert(g_context_calls == 1);

   char *next = ingress_preinject_build("document billing retry policy", 0);
   assert(next && strstr(next, "task-conditioned code") != NULL);
   free(next);
   assert(g_context_calls == 2);

   /* Observe retrieves and validates the packet but preserves the existing
    * project-local preview bytes. */
   ingress_preinject_task_state_reset();
   g_context_mode = "observe";
   char *observed = ingress_preinject_build("fix local resolver", 0);
   assert(observed && strstr(observed, "recommended (code):") != NULL);
   assert(strstr(observed, "task-conditioned code") == NULL);
   assert(strstr(observed, "global preference") != NULL);
   free(observed);
   assert(g_context_calls == 3);

   ingress_preinject_set_session_id(NULL);
   g_facts_enabled = 0;
   g_context_mode = "observe";
   printf("task_context_mode_and_first_turn_gate OK\n");
}

/* An empty recall and an unreachable knowledge service produce the SAME envelope
 * -- no memory previews either way -- so nothing downstream can tell them apart.
 * That is how an agent ends up reporting that a symbol does not exist when it
 * merely could not look. Assert the counter moves on the outage and stays put on
 * the quiet turn; the envelope itself must be identical in both cases, because
 * those bytes are a cache prefix and must not change during an outage. */
static void test_recall_unavailable_is_counted_apart_from_empty(void)
{
   ingress_preinject_task_state_reset();
   ingress_preinject_set_session_id("session-degraded");
   g_context_mode = "off"; /* isolate the memory layer from the task-context path */
   g_memory_returns_none = 1;

   /* Quiet turn: recall reached the service and it had nothing. Not a failure. */
   g_context_result = KB_CLIENT_RESULT_OK;
   long long before_quiet = ingress_preinject_recall_unavailable_total();
   char *quiet = ingress_preinject_build("what did we decide about retries", 0);
   assert(ingress_preinject_recall_unavailable_total() == before_quiet);

   /* Outage: same empty result, different cause. This must be counted. */
   g_context_result = KB_CLIENT_RESULT_UNAVAILABLE;
   char *outage = ingress_preinject_build("what did we decide about retries", 0);
   assert(ingress_preinject_recall_unavailable_total() == before_quiet + 1);

   /* The envelope is unchanged by the outage: whatever the quiet turn produced,
    * the degraded turn produces byte-for-byte. The signal is the counter and the
    * log line, never the provider-visible request. */
   if (quiet == NULL)
      assert(outage == NULL);
   else
   {
      assert(outage != NULL);
      assert(strcmp(quiet, outage) == 0);
   }
   free(quiet);
   free(outage);

   /* Recovery does not keep counting. */
   g_context_result = KB_CLIENT_RESULT_OK;
   long long before_recovery = ingress_preinject_recall_unavailable_total();
   char *recovered = ingress_preinject_build("what did we decide about retries", 0);
   assert(ingress_preinject_recall_unavailable_total() == before_recovery);
   free(recovered);

   g_memory_returns_none = 0;
   g_context_mode = "observe";
   ingress_preinject_set_session_id(NULL);
   printf("  ok    memory recall: outage counted apart from an empty result\n");
}

static void test_unavailable_task_context_retries_after_recovery(void)
{
   ingress_preinject_task_state_reset();
   ingress_preinject_set_session_id("session-recovery");
   g_context_mode = "on";
   g_context_calls = 0;
   g_context_result = KB_CLIENT_RESULT_UNAVAILABLE;

   char *outage = ingress_preinject_build("fix local resolver", 0);
   assert(outage == NULL);
   assert(g_context_calls == 1);

   /* Same-task vocabulary is eligible again because unavailable is not an
    * abstention/empty result. The KB breaker owns the actual retry rate. */
   g_context_result = KB_CLIENT_RESULT_OK;
   char *recovered = ingress_preinject_build("please fix the local resolver", 0);
   assert(recovered && strstr(recovered, "task-conditioned code") != NULL);
   free(recovered);
   assert(g_context_calls == 2);

   ingress_preinject_set_session_id(NULL);
   g_context_mode = "observe";
   g_context_result = KB_CLIENT_RESULT_OK;
   printf("unavailable_task_context_retries_after_recovery OK\n");
}

static void test_query_from_messages(void)
{
   /* String content; last user message wins over an earlier one. */
   cJSON *m = cJSON_Parse("[{\"role\":\"user\",\"content\":\"first\"},"
                          "{\"role\":\"assistant\",\"content\":\"mid\"},"
                          "{\"role\":\"user\",\"content\":\"second ask\"}]");
   char *q = ingress_preinject_query_from_messages(m);
   assert(q && strcmp(q, "second ask") == 0);
   free(q);
   cJSON_Delete(m);

   /* Array-of-parts content. */
   cJSON *m2 =
       cJSON_Parse("[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"hello \"},"
                   "{\"type\":\"input_text\",\"text\":\"world\"}]}]");
   char *q2 = ingress_preinject_query_from_messages(m2);
   assert(q2 && strcmp(q2, "hello world") == 0);
   free(q2);
   cJSON_Delete(m2);

   /* No user message -> NULL. */
   cJSON *m3 = cJSON_Parse("[{\"role\":\"assistant\",\"content\":\"hi\"}]");
   assert(ingress_preinject_query_from_messages(m3) == NULL);
   cJSON_Delete(m3);

   assert(ingress_preinject_query_from_messages(NULL) == NULL);
   printf("query_from_messages OK\n");
}

static void test_apply(void)
{
   /* NULL/blank envelope -> copy of instructions (or NULL). */
   char *a = ingress_preinject_apply("SYS", NULL);
   assert(a && strcmp(a, "SYS") == 0);
   free(a);
   assert(ingress_preinject_apply(NULL, NULL) == NULL);
   char *blank = ingress_preinject_apply("SYS", "   \n ");
   assert(blank && strcmp(blank, "SYS") == 0);
   free(blank);

   /* Envelope prepended, separated from instructions. */
   char *m = ingress_preinject_apply("SYSTEM PROMPT", "<aimee-context>...</aimee-context>");
   assert(m != NULL);
   assert(strstr(m, "<aimee-context>") == m);
   assert(strstr(m, "SYSTEM PROMPT") != NULL);
   assert(strstr(m, "</aimee-context>\n\nSYSTEM PROMPT") != NULL); /* separated */
   free(m);

   /* Envelope with NULL instructions -> just the envelope. */
   char *o = ingress_preinject_apply(NULL, "ENV");
   assert(o && strstr(o, "ENV") == o);
   free(o);
   printf("apply OK\n");
}

/* Cache-prefix placement (§2): append puts the stable instructions first and the
 * volatile envelope last (mirror of apply). */
static void test_append(void)
{
   /* NULL/blank envelope -> copy of instructions (or NULL). */
   char *a = ingress_preinject_append("SYS", NULL);
   assert(a && strcmp(a, "SYS") == 0);
   free(a);
   assert(ingress_preinject_append(NULL, NULL) == NULL);

   /* Instructions first, envelope appended, separated by a blank line. */
   char *m = ingress_preinject_append("SYSTEM PROMPT", "<aimee-context>...</aimee-context>");
   assert(m != NULL);
   assert(strstr(m, "SYSTEM PROMPT") == m);                       /* prefix stays at front */
   assert(strstr(m, "SYSTEM PROMPT\n\n<aimee-context>") != NULL); /* envelope is the suffix */
   free(m);

   /* NULL instructions -> just the envelope. */
   char *o = ingress_preinject_append(NULL, "ENV");
   assert(o && strcmp(o, "ENV") == 0);
   free(o);
   printf("append OK\n");
}

static void test_format_code_block(void)
{
   assert(ingress_preinject_format_code_block(NULL, 3) == NULL);
   code_search_hit_t hits[2];
   memset(hits, 0, sizeof(hits));
   assert(ingress_preinject_format_code_block(hits, 0) == NULL);

   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/server/openai_chat.c");
   snprintf(hits[0].snippet, sizeof(hits[0].snippet),
            "  static int   responses_stream_handler(\n\tconst char *body)");
   snprintf(hits[1].file_path, sizeof(hits[1].file_path), "src/server/ingress_preinject.c");
   hits[1].snippet[0] = '\0'; /* no snippet -> just the file line */

   char *b = ingress_preinject_format_code_block(hits, 2);
   assert(b != NULL);
   assert(strstr(b, "recommended (code):") == b);
   assert(strstr(b, "  - src/server/openai_chat.c") != NULL);
   assert(strstr(b, "  - src/server/ingress_preinject.c") != NULL);
   /* snippet whitespace collapsed to a single line, no tabs/newlines */
   assert(strstr(b, "> static int responses_stream_handler( const char *body)") != NULL);
   assert(strchr(b, '\t') == NULL);
   free(b);
   printf("format_code_block OK\n");
}

static void test_budgeted_build_uses_memory_previews(void)
{
   char *env = ingress_preinject_build("deploy matrix", 0);
   assert(env != NULL);
   assert(strlen(env) <= 1200);
   assert(strstr(env, "recommended (memory previews):") != NULL);
   assert(strstr(env, "memory:101") != NULL);
   assert(strstr(env, "Use the deploy matrix.") != NULL);
   assert(strstr(env, "context-budget:") != NULL);
   /* "memory_get" used to appear only as part of the explore-with line, which is
    * no longer in the per-turn envelope -- see the golden below. */
   assert(strstr(env, "Fallback preview from content.") != NULL);

   /* P0 byte-equivalence anchor: the Envelope IR refactor must reproduce the
    * pre-refactor envelope byte for byte for this fixed stub scenario (code hit
    * + two memory previews under the 1200-byte budget). Captured from the live
    * pre-refactor code. */
   static const char *GOLDEN =
       "<aimee-context confidence=\"medium\">\n"
       "recommended (code):\n"
       "  - src/server/ingress_preinject.c\n"
       "    > builder emits a bounded context envelope\n"
       "\n"
       "recommended (memory previews):\n"
       "  - memory:101 deploy path [L2/fact score=0.880 headline_missing=false]\n"
       "    > Use the deploy matrix.\n"
       "  - memory:102 fallback [L2/policy score=0.440 headline_missing=true]\n"
       "    > Fallback preview from content.\n"
       "context-budget: used_bytes=342 budget_bytes=1200 omitted_count=0 headline_missing_count=1\n"
       /* explore-with / fix-scope USED TO BE HERE. They moved to a session-start
        * injection on the IR (ir_stage_memory), because riding the per-turn
        * retrieval envelope meant aimee only told an agent to use aimee's tools
        * once it had already retrieved something -- on an unindexed repo the agent
        * got no guidance at all and reached for shell. They are also no longer
        * repeated every turn, which is what these bytes were charging for. */
       "</aimee-context>";
   assert(strcmp(env, GOLDEN) == 0);
   free(env);
   printf("budgeted_build_uses_memory_previews OK\n");
}

static void test_build_requires_confidence_provider(void)
{
   ingress_preinject_task_state_reset();
   ingress_preinject_register_confidence_provider(NULL);
   char *env = ingress_preinject_build("deploy matrix", 0);
   assert(env == NULL);
   ingress_preinject_register_confidence_provider(test_confidence_provider);
   ingress_preinject_task_state_reset();
   printf("build_requires_confidence_provider OK\n");
}

/* P0 Envelope IR: the renderer reproduces the old inline rendering — group
 * headers, single blank-line separators between non-empty groups, the
 * header-rides-the-first-fitting-candidate rule, the budget/omitted gate, the
 * footer, and the truncation note. Synthetic entries keep the expected bytes
 * easy to compute. The renderer frees nothing it is given. */
static void test_render_block(void)
{
   /* Two groups + footer, no omission: exact bytes. block_budget = 1000-384. */
   {
      ingress_entry_t e[2] = {
          {ING_SRC_CODE, ING_XF_NONE, "C:\n", strdup("a\n")},
          {ING_SRC_MEMORY, ING_XF_NONE, "M:\n", strdup("b\n")},
      };
      int omitted = -1;
      char *blk = ingress_render_block(e, 2, 1000, 0, &omitted);
      assert(blk != NULL);
      assert(strcmp(blk, "C:\na\n\nM:\nb\ncontext-budget: used_bytes=11 budget_bytes=1000 "
                         "omitted_count=0 headline_missing_count=0\n") == 0);
      assert(omitted == 0);
      free(blk);
      free(e[0].preview);
      free(e[1].preview);
   }

   /* Header rides the first candidate that fits: a too-big first code entry is
    * omitted, the header appears on the second (fitting) one. block_budget = 10
    * (envelope_budget 394) is too small for the footer/trunc, so neither lands. */
   {
      ingress_entry_t e[2] = {
          {ING_SRC_CODE, ING_XF_NONE, "C:\n", strdup("AAAAAAAAAA\n")}, /* 3+11 > 10 */
          {ING_SRC_CODE, ING_XF_NONE, "C:\n", strdup("x\n")},          /* 3+2  <= 10 */
      };
      int omitted = -1;
      char *blk = ingress_render_block(e, 2, 394, 0, &omitted);
      assert(blk != NULL);
      assert(strcmp(blk, "C:\nx\n") == 0); /* header on the fitting entry, once */
      assert(omitted == 1);
      free(blk);
      free(e[0].preview);
      free(e[1].preview);
   }

   /* Separator suppression: when the first group appends nothing, the next group
    * gets no leading blank line (matches the old `if (block.len)` guard). */
   {
      ingress_entry_t e[2] = {
          {ING_SRC_CODE, ING_XF_NONE, "C:\n", strdup("AAAAAAAAAA\n")}, /* omitted */
          {ING_SRC_MEMORY, ING_XF_NONE, "M:\n", strdup("y\n")},        /* fits */
      };
      int omitted = -1;
      char *blk = ingress_render_block(e, 2, 394, 0, &omitted);
      assert(blk != NULL);
      assert(strcmp(blk, "M:\ny\n") == 0); /* no leading "\n" */
      assert(omitted == 1);
      free(blk);
      free(e[0].preview);
      free(e[1].preview);
   }

   /* Empty list -> nothing rendered (NULL or ""), no footer. */
   {
      int omitted = -1;
      char *blk = ingress_render_block(NULL, 0, 1000, 0, &omitted);
      assert(blk == NULL || blk[0] == '\0');
      assert(omitted == 0);
      free(blk);
   }
   printf("render_block OK\n");
}

/* Auditable-correctness P1: the per-turn retrieval-event id seam. */
static void test_turn_id_mint_and_thread_local(void)
{
   /* mint produces a canonical 8-4-4-4-12 UUID. */
   char a[40], b[40];
   ingress_preinject_mint_turn_id(a, sizeof(a));
   ingress_preinject_mint_turn_id(b, sizeof(b));
   assert(strlen(a) == 36);
   assert(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
   for (int i = 0; a[i]; i++)
   {
      char c = a[i];
      assert(c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
   }
   assert(strcmp(a, b) != 0); /* random — two mints differ */

   /* the thread-local set/get round-trips and clears on NULL/"" */
   assert(ingress_preinject_turn_id()[0] == '\0'); /* unset by default */
   ingress_preinject_set_turn_id("turn-xyz");
   assert(strcmp(ingress_preinject_turn_id(), "turn-xyz") == 0);
   ingress_preinject_set_turn_id(NULL);
   assert(ingress_preinject_turn_id()[0] == '\0');
   ingress_preinject_set_turn_id("turn-2");
   ingress_preinject_set_turn_id("");
   assert(ingress_preinject_turn_id()[0] == '\0');
   printf("turn_id_mint_and_thread_local OK\n");
}

/* P1b lossy code fold: default-off keeps the snippet; compress-on replaces it with
 * a `file:line` reference under a code_span_get-expandable header; X-Aimee-Compress:0
 * (request context) overrides back to the snippet. */
static void test_compress_code_fold(void)
{
   request_context_clear();

   /* Compression OFF (default): the code entry keeps its snippet preview and the
    * plain header — byte-for-byte the pre-compression behaviour. */
   g_test_compress = 0;
   char *off = ingress_preinject_build("how does the builder work", 0);
   assert(off != NULL);
   assert(strstr(off, "recommended (code):\n") != NULL);
   assert(strstr(off, "    > ") != NULL);                 /* snippet present */
   assert(strstr(off, "ingress_preinject.c\n") != NULL);  /* file line, no :line */
   assert(strstr(off, "ingress_preinject.c:42") == NULL); /* not folded */
   free(off);

   /* Compression ON: the snippet is folded to a `file:line` reference under the
    * expandable header, and the raw snippet text is gone. */
   g_test_compress = 1;
   char *on = ingress_preinject_build("how does the builder work", 0);
   assert(on != NULL);
   assert(strstr(on, "recommended (code — expand via code_span_get):\n") != NULL);
   assert(strstr(on, "ingress_preinject.c:42") != NULL);       /* folded reference */
   assert(strstr(on, "typed entry list and renders") == NULL); /* snippet body dropped */
   free(on);

   /* Per-request X-Aimee-Compress:0 overrides the config flag back to no fold. */
   request_context_t rc;
   memset(&rc, 0, sizeof(rc));
   rc.compress_disabled = 1;
   request_context_set(&rc);
   char *ovr = ingress_preinject_build("how does the builder work", 0);
   assert(ovr != NULL);
   assert(strstr(ovr, "ingress_preinject.c:42") == NULL); /* override -> not folded */
   assert(strstr(ovr, "    > ") != NULL);                 /* snippet restored */
   free(ovr);
   request_context_clear();

   g_test_compress = 0;
   printf("compress_code_fold OK\n");
}

int main(void)
{
   printf("ingress_preinject: ");
   test_confidence_tiers();
   test_format_envelope();
   test_format_task_context_strict_contract();
   test_task_context_mode_and_first_turn_gate();
   test_unavailable_task_context_retries_after_recovery();
   test_recall_unavailable_is_counted_apart_from_empty();
   test_format_code_block();
   test_query_from_messages();
   test_apply();
   test_append();
   test_render_block();
   test_budgeted_build_uses_memory_previews();
   test_build_requires_confidence_provider();
   test_turn_id_mint_and_thread_local();
   test_compress_code_fold();
   printf("all tests passed\n");
   return 0;
}

const char *config_embedder_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   if (cfg && cfg->embedder_command[0])
      return cfg->embedder_command;
   return MEMORY_EMBED_TEST_FIXTURE;
}
