/* test_curator_fixtures.c: well-formedness + behavioral checks for the
 * deep-curator fixture corpus under benchmarks/curator/fixtures/ (AC#16).
 *
 * The three files exist in the charter's three shapes (positive,
 * false_positive, regression). This test does two things so the corpus is
 * live data, not dead files:
 *   1. schema validation — every line parses, carries the common keys, and
 *      carries the per-pass keys for its declared `pass`.
 *   2. behavioral grounding — every extract_code_unit fixture is shaped by the
 *      production curator seam and decided by the process-module parity handler;
 *      its `expected_grounding` label must match the module decision.
 *
 * CURATOR_FIXTURE_DIR is injected at compile time (see tests/Rules.mk). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/core/event_bus/module_runtime.h>
#include "cJSON.h"
#include "kb_curator_grounding.h"

#ifndef CURATOR_FIXTURE_DIR
#define CURATOR_FIXTURE_DIR "../benchmarks/curator/fixtures"
#endif

#define MAX_CALLEES 64

typedef struct
{
   int total;
   int n_doc;
   int n_code;
   int n_bridge;
   int n_code_reject;
} fx_stats_t;

extern aimee_module_status_t aimee_kb_synthesis_module_handler(const aimee_module_invocation_t *,
                                                               const uint8_t *, uint32_t, uint8_t *,
                                                               uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int grounding_module_provider(aimee_kb_synthesis_claim_kind_t claim_kind,
                                     const char *const *claims, uint32_t claim_count,
                                     const char *const *callees, uint32_t callee_count,
                                     aimee_kb_synthesis_grounding_decision_t *decision)
{
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
   uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_KB_SYNTHESIS_STAGE_GROUNDING};
   if (aimee_kb_synthesis_request_encode(claim_kind, claims, claim_count, callees, callee_count,
                                         request, sizeof(request)) != 0 ||
       aimee_kb_synthesis_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_kb_synthesis_response_decode(response, response_len, decision);
}

/* Read an entire file into a malloc'd NUL-terminated buffer (caller frees). */
static char *read_file(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
   {
      fprintf(stderr, "  FAIL: cannot open fixture file %s\n", path);
      return NULL;
   }
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   buf[n] = '\0';
   fclose(fp);
   return buf;
}

static const char *req_string(const cJSON *obj, const char *key, const char *id)
{
   const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsString(j) || !j->valuestring[0])
   {
      fprintf(stderr, "  FAIL: fixture %s missing string key '%s'\n", id ? id : "?", key);
      assert(0 && "missing required string key");
   }
   return j->valuestring;
}

static const cJSON *req_array(const cJSON *obj, const char *key, const char *id)
{
   const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsArray(j))
   {
      fprintf(stderr, "  FAIL: fixture %s missing array key '%s'\n", id ? id : "?", key);
      assert(0 && "missing required array key");
   }
   return j;
}

static void validate_code_unit(const cJSON *obj, const char *id, fx_stats_t *st)
{
   req_string(obj, "symbol", id);
   const cJSON *callees_j = req_array(obj, "callees", id);
   const cJSON *se_j = req_array(obj, "claimed_side_effects", id);
   const char *expected = req_string(obj, "expected_grounding", id);
   assert((strcmp(expected, "reject") == 0 || strcmp(expected, "commit") == 0) &&
          "expected_grounding must be reject|commit");

   /* Build a code_unit payload carrying the claimed side_effects, then run the
    * production request-shaping seam against the process-module handler. */
   cJSON *payload = cJSON_CreateObject();
   cJSON_AddItemToObject(payload, "side_effects", cJSON_Duplicate(se_j, 1));

   const char *callees[MAX_CALLEES];
   int n = 0;
   const cJSON *c = NULL;
   cJSON_ArrayForEach(c, callees_j)
   {
      if (cJSON_IsString(c) && n < MAX_CALLEES)
         callees[n++] = c->valuestring;
   }

   aimee_kb_synthesis_grounding_decision_t decision;
   assert(kb_curator_grounding_decide(payload, callees, (uint32_t)n, &decision) == 0);
   cJSON_Delete(payload);

   int want_reject = (strcmp(expected, "reject") == 0);
   if (decision.contradicts != want_reject)
   {
      fprintf(stderr, "  FAIL: fixture %s expected_grounding=%s but module says %s (reason='%s')\n",
              id, expected, decision.contradicts ? "reject" : "commit", decision.reason);
      assert(0 && "grounding label mismatch");
   }
   if (want_reject)
   {
      assert(decision.reason[0] != '\0' && "reject must name an offending callee");
      st->n_code_reject++;
   }
   st->n_code++;
}

static void validate_doc(const cJSON *obj, const char *id, fx_stats_t *st)
{
   req_string(obj, "text", id);
   const cJSON *exp = cJSON_GetObjectItemCaseSensitive(obj, "expected");
   assert(cJSON_IsObject(exp) && "extract_doc needs an 'expected' object");
   req_string(exp, "status", id);
   req_string(exp, "priority", id);
   const cJSON *comps = req_array(exp, "components", id);
   assert(cJSON_GetArraySize(comps) >= 1 && "components must be non-empty");
   st->n_doc++;
}

static void validate_bridge(const cJSON *obj, const char *id, fx_stats_t *st)
{
   req_string(obj, "topic", id);
   const cJSON *files = req_array(obj, "expected_files", id);
   assert(cJSON_GetArraySize(files) >= 1 && "expected_files must be non-empty");
   st->n_bridge++;
}

static void validate_file(const char *shape, fx_stats_t *st)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s.jsonl", CURATOR_FIXTURE_DIR, shape);
   char *buf = read_file(path);
   assert(buf != NULL);

   memset(st, 0, sizeof(*st));
   char *save = NULL;
   for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      /* Skip blank lines. */
      char *q = line;
      while (*q == ' ' || *q == '\t' || *q == '\r')
         q++;
      if (!*q)
         continue;

      cJSON *obj = cJSON_Parse(line);
      if (!obj)
      {
         fprintf(stderr, "  FAIL: invalid JSON line in %s.jsonl: %.80s\n", shape, line);
         assert(0 && "fixture line is not valid JSON");
      }

      const char *id = req_string(obj, "id", NULL);
      const char *pass = req_string(obj, "pass", id);
      const char *fx_shape = req_string(obj, "shape", id);
      req_string(obj, "notes", id);
      assert(strcmp(fx_shape, shape) == 0 && "fixture 'shape' must match its file");

      if (strcmp(pass, "extract_code_unit") == 0)
         validate_code_unit(obj, id, st);
      else if (strcmp(pass, "extract_doc") == 0)
         validate_doc(obj, id, st);
      else if (strcmp(pass, "bridge") == 0)
         validate_bridge(obj, id, st);
      else
      {
         fprintf(stderr, "  FAIL: fixture %s has unknown pass '%s'\n", id, pass);
         assert(0 && "unknown pass");
      }

      st->total++;
      cJSON_Delete(obj);
   }
   free(buf);
}

int main(void)
{
   printf("curator_fixtures:\n");
   kb_curator_grounding_register_provider(grounding_module_provider);

   fx_stats_t pos, fp, reg;
   validate_file("positive", &pos);
   validate_file("false_positive", &fp);
   validate_file("regression", &reg);

   /* Coverage floors — keep the corpus from silently shrinking below the
    * charter's "three shapes, all passes" expectation. */
   assert(pos.total >= 9 && "positive corpus too small");
   assert(pos.n_doc >= 3 && pos.n_code >= 3 && pos.n_bridge >= 3 &&
          "positive must cover all three passes");
   printf("  PASS: positive (%d lines: %d doc, %d code, %d bridge)\n", pos.total, pos.n_doc,
          pos.n_code, pos.n_bridge);

   assert(fp.total >= 6 && "false_positive corpus too small");
   printf("  PASS: false_positive (%d lines)\n", fp.total);

   assert(reg.total >= 6 && "regression corpus too small");
   assert(reg.n_code_reject >= 2 && "regression must encode >=2 grounding-reject cases");
   printf("  PASS: regression (%d lines, %d grounding-reject cases)\n", reg.total,
          reg.n_code_reject);

   printf("ok\n");
   return 0;
}
