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
int kb_client_memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   (void)query;
   (void)limit;
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
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   if (!out || max <= 0)
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

static void test_confidence_tiers(void)
{
   assert(strcmp(ingress_preinject_confidence(0.9), "high") == 0);
   assert(strcmp(ingress_preinject_confidence(0.66), "high") == 0);
   assert(strcmp(ingress_preinject_confidence(0.5), "medium") == 0);
   assert(strcmp(ingress_preinject_confidence(0.33), "medium") == 0);
   assert(strcmp(ingress_preinject_confidence(0.1), "low") == 0);
   assert(strcmp(ingress_preinject_confidence(0.0), "low") == 0);
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
   assert(strstr(e, "explore-with: find_symbol") != NULL);
   assert(strstr(e, "</aimee-context>") != NULL);
   free(e);

   /* Missing confidence defaults to low. */
   char *e2 = ingress_preinject_format_envelope("x", NULL);
   assert(e2 && strstr(e2, "confidence=\"low\"") != NULL);
   free(e2);
   printf("format_envelope OK\n");
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
   assert(strstr(env, "memory_get") != NULL);
   assert(strstr(env, "Fallback preview from content.") != NULL);
   free(env);
   printf("budgeted_build_uses_memory_previews OK\n");
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

int main(void)
{
   printf("ingress_preinject: ");
   test_confidence_tiers();
   test_format_envelope();
   test_format_code_block();
   test_query_from_messages();
   test_apply();
   test_budgeted_build_uses_memory_previews();
   test_turn_id_mint_and_thread_local();
   printf("all tests passed\n");
   return 0;
}
