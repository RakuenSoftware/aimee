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
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   (void)out;
   (void)max;
   return 0;
}
int config_load(config_t *cfg)
{
   if (cfg)
      memset(cfg, 0, sizeof(*cfg));
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
   cJSON *m2 = cJSON_Parse(
       "[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"hello \"},"
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
   snprintf(hits[0].snippet, sizeof(hits[0].snippet), "  static int   responses_stream_handler(\n\tconst char *body)");
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

int main(void)
{
   printf("ingress_preinject: ");
   test_confidence_tiers();
   test_format_envelope();
   test_format_code_block();
   test_query_from_messages();
   test_apply();
   printf("all tests passed\n");
   return 0;
}
