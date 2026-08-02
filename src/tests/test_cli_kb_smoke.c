/* test_cli_kb_smoke.c: the health-derived verdicts of `aimee kb smoke`.
 *
 * These checks read a kb.health payload, and the first version of this command read
 * kb_health_t's field names out of it instead. Those are different shapes: the
 * struct is what the server fills in internally, and the JSON uses none of its
 * names. The result was a confident FAIL on "DB2 schema present" and "vector store
 * ready" against a kb that was perfectly healthy, which is the worst possible
 * failure for a diagnostic: it sends someone to debug storage that is fine.
 *
 * So the payloads below are real responses, not invented ones. The healthy and
 * failed-queue cases are the shape a live kb actually returned.
 */
#include "cli_kb_smoke.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A real kb.health response. Vector store up, nothing embedded, every queued job
 * failed. Trimmed of fields none of these checks read. */
static const char *PAYLOAD_LIVE =
    "{\"summary_status\":\"ok\",\"available\":true,\"files\":1,\"chunks\":13,"
    "\"embeddings\":0,"
    "\"queue\":{\"pending\":0,\"running\":0,\"done\":0,\"failed\":25,\"total\":25},"
    "\"vector\":{\"available\":true,\"backend\":\"pgvector\","
    "\"memory_collection_ready\":true,\"kb_collection_ready\":true,"
    "\"memory_points\":332,\"kb_points\":13,\"status\":\"ok\"}}";

static const char *PAYLOAD_HEALTHY =
    "{\"summary_status\":\"ok\",\"available\":true,\"embeddings\":4096,"
    "\"queue\":{\"failed\":0,\"total\":12},"
    "\"vector\":{\"available\":true,\"backend\":\"pgvector\","
    "\"memory_collection_ready\":true,\"kb_collection_ready\":true,"
    "\"memory_points\":10,\"kb_points\":20}}";

/* The shape that broke the first implementation: a healthy kb whose JSON simply does
 * not carry the struct's field names. Nothing here may be read as a failure. */
static const char *PAYLOAD_NO_STRUCT_FIELDS =
    "{\"summary_status\":\"ok\",\"available\":true,\"embeddings\":1,"
    "\"queue\":{\"failed\":0,\"total\":1},"
    "\"vector\":{\"available\":true,\"backend\":\"pgvector\","
    "\"memory_collection_ready\":true,\"kb_collection_ready\":true}}";

static const char *PAYLOAD_VECTOR_DOWN =
    "{\"summary_status\":\"degraded\",\"available\":true,\"embeddings\":10,"
    "\"queue\":{\"failed\":0,\"total\":3},"
    "\"vector\":{\"available\":false,\"backend\":\"pgvector\","
    "\"memory_collection_ready\":false,\"kb_collection_ready\":false}}";

static int failures = 0;

static const char *outcome_of(cJSON *rows, const char *check)
{
   cJSON *row = NULL;
   cJSON_ArrayForEach(row, rows)
   {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(row, "check");
      if (n && cJSON_IsString(n) && strcmp(n->valuestring, check) == 0)
      {
         cJSON *o = cJSON_GetObjectItemCaseSensitive(row, "outcome");
         return (o && cJSON_IsString(o)) ? o->valuestring : "";
      }
   }
   return "<absent>";
}

static void expect(cJSON *rows, const char *check, const char *want)
{
   const char *got = outcome_of(rows, check);
   if (strcmp(got, want) != 0)
   {
      printf("  FAIL  %-22s expected %s, got %s\n", check, want, got);
      failures++;
      return;
   }
   printf("  ok    %-22s %s\n", check, got);
}

static cJSON *eval(const char *json, int *p, int *f, int *s)
{
   cJSON *health = json ? cJSON_Parse(json) : NULL;
   if (json)
      assert(health && "test payload must parse");
   cJSON *rows = cli_kb_smoke_eval_health(health, p, f, s);
   if (health)
      cJSON_Delete(health);
   assert(rows);
   return rows;
}

int main(void)
{
   int p, f, s;
   cJSON *rows;

   printf("live payload (vector up, nothing embedded, whole queue failed)\n");
   rows = eval(PAYLOAD_LIVE, &p, &f, &s);
   expect(rows, "kb is reachable", "pass");
   /* The regression: this must not be a failure just because the JSON has no
    * pgvec_ok. It reports the vector block the response really carries. */
   expect(rows, "vector store ready", "pass");
   /* Supported state, so a skip. An un-embedded corpus is not broken. */
   expect(rows, "corpus is embedded", "skip");
   /* The one thing genuinely wrong here, and the only thing that should fail. */
   expect(rows, "ingest queue healthy", "fail");
   if (f != 1)
   {
      printf("  FAIL  failed count: expected 1, got %d\n", f);
      failures++;
   }
   cJSON_Delete(rows);

   printf("healthy payload\n");
   rows = eval(PAYLOAD_HEALTHY, &p, &f, &s);
   expect(rows, "vector store ready", "pass");
   expect(rows, "corpus is embedded", "pass");
   expect(rows, "ingest queue healthy", "pass");
   if (f != 0)
   {
      printf("  FAIL  healthy payload reported %d failure(s)\n", f);
      failures++;
   }
   cJSON_Delete(rows);

   printf("payload carrying none of kb_health_t's field names\n");
   rows = eval(PAYLOAD_NO_STRUCT_FIELDS, &p, &f, &s);
   if (f != 0)
   {
      printf("  FAIL  a healthy kb reported %d failure(s); the checks are reading\n"
             "        field names this response does not use\n",
             f);
      failures++;
   }
   else
      printf("  ok    no spurious failures\n");
   cJSON_Delete(rows);

   printf("vector store down\n");
   rows = eval(PAYLOAD_VECTOR_DOWN, &p, &f, &s);
   expect(rows, "vector store ready", "fail");
   cJSON_Delete(rows);

   printf("unreachable kb\n");
   rows = eval(NULL, &p, &f, &s);
   expect(rows, "kb is reachable", "fail");
   /* Nothing else may be asserted about a kb that never answered. */
   if (cJSON_GetArraySize(rows) != 1)
   {
      printf("  FAIL  unreachable kb produced %d row(s); it can only know one thing\n",
             cJSON_GetArraySize(rows));
      failures++;
   }
   cJSON_Delete(rows);

   if (failures)
   {
      printf("\ncli_kb_smoke: %d check(s) failed\n", failures);
      return 1;
   }
   printf("\ncli_kb_smoke: all verdicts correct\n");
   return 0;
}
