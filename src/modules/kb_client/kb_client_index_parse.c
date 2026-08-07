/* kb_client_index_parse.c: response-shape adapter for index.scan.
 *
 * Extracted from kb_client_index.c so the wire contract — particularly
 * "status:error must surface, never be silently flattened to projects:0" —
 * can be unit-tested without linking the cli transport. The bug this
 * guards: an error response from aimee-kb used to be parsed into a
 * successful empty scan, hiding real DB2/canonical-index failures. */
#include "cJSON.h"
#include "json_fluent.h"
#include "kb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build the wire-level response object for an index.scan call from the
 * kb_client_index_scan result. Caller takes ownership of the returned
 * cJSON. Centralised so the dispatch path and tests share one truth. */
/* Tunable because "large" has no fixed size: a ~3000-file checkout on a busy kb
 * crosses five minutes, and when it does the POST below returns nothing and the
 * caller reports the service as unavailable -- while the kb is still scanning,
 * holding the db2 connection it leased. Operators scanning big trees need to
 * raise this rather than watch every scan report a healthy service as down. */
/* Read timeout for the code-index query routes.
 *
 * This was a hardcoded 5s. Measured against a 3825-file checkout on CT403, the
 * kb answers /v1/code/blast-radius in 5.8-5.9s consistently -- it walks the
 * dependency graph, not a single row -- so EVERY blast-radius lookup missed the
 * deadline by under a second and surfaced as "blast radius lookup failed" with
 * http=-1. Symbol and caller lookups are far cheaper and stayed under it, which
 * is why two of three readiness probes passed and the third never did.
 *
 * The failure scaled with corpus size, so it was invisible on small fixtures and
 * total on real ones. Default generously and let an operator tune it, matching
 * the scan timeout above. */
int kb_client_index_read_timeout_ms(void)
{
   const char *env = getenv("AIMEE_KB_READ_TIMEOUT_MS");
   if (env && env[0])
   {
      long v = strtol(env, NULL, 10);
      if (v > 0 && v <= 24L * 60 * 60 * 1000)
         return (int)v;
   }
   return (60 * 1000);
}

int kb_client_index_scan_timeout_ms(void)
{
   const char *env = getenv("AIMEE_KB_SCAN_TIMEOUT_MS");
   if (env && env[0])
   {
      long v = strtol(env, NULL, 10);
      if (v > 0 && v <= 24L * 60 * 60 * 1000)
         return (int)v;
   }
   return (5 * 60 * 1000);
}

void *kb_client_index_scan_format_response(int kb_rc, const kb_client_index_scan_result_t *res)
{
   cJSON *resp;
   if (kb_rc != 0)
   {
      const char *msg = "knowledge service unavailable";
      if (res && strcmp(res->reason, "error") == 0 && res->message[0])
         msg = res->message;
      resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", msg);
      return resp;
   }

   resp = jo_ok();
   if (res)
   {
      cJSON_AddNumberToObject(resp, "projects", res->projects);
      cJSON_AddNumberToObject(resp, "files", res->files);
      if (res->inspected > 0)
         cJSON_AddNumberToObject(resp, "inspected", res->inspected);
      cJSON_AddBoolToObject(resp, "skipped", res->skipped);
      if (res->reason[0])
         jo_add_str(resp, "reason", res->reason);
      if (res->retry_after > 0)
         cJSON_AddNumberToObject(resp, "retry_after", (double)res->retry_after);
   }
   return resp;
}

int kb_client_index_scan_apply_response(const void *resp_v, kb_client_index_scan_result_t *out)
{
   const cJSON *resp = (const cJSON *)resp_v;
   if (out)
      memset(out, 0, sizeof(*out));

   if (!resp)
   {
      if (out)
      {
         out->skipped = 1;
         snprintf(out->reason, sizeof(out->reason), "no_kb");
      }
      return -1;
   }

   const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      if (out)
      {
         out->skipped = 1;
         snprintf(out->reason, sizeof(out->reason), "error");
         const cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(msg) && msg->valuestring[0])
            snprintf(out->message, sizeof(out->message), "%s", msg->valuestring);
      }
      return -1;
   }

   if (out)
   {
      const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "skipped");
      const cJSON *reason = cJSON_GetObjectItemCaseSensitive(resp, "reason");
      const cJSON *retry = cJSON_GetObjectItemCaseSensitive(resp, "retry_after");
      const cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
      const cJSON *files = cJSON_GetObjectItemCaseSensitive(resp, "files");
      const cJSON *inspected = cJSON_GetObjectItemCaseSensitive(resp, "inspected");
      out->skipped = cJSON_IsTrue(skipped) ? 1 : 0;
      if (cJSON_IsString(reason))
         snprintf(out->reason, sizeof(out->reason), "%s", reason->valuestring);
      if (cJSON_IsNumber(retry))
         out->retry_after = (long)retry->valuedouble;
      if (cJSON_IsNumber(projects))
         out->projects = (int)projects->valuedouble;
      if (cJSON_IsNumber(files))
         out->files = (int)files->valuedouble;
      if (cJSON_IsNumber(inspected))
         out->inspected = (int)inspected->valuedouble;
   }
   return 0;
}

/* Blast-radius CONTRACT validation, kept here rather than beside the transport
 * so it can be exercised against a recorded kb payload without a live kb.
 *
 * This split is not cosmetic. The lookup used to fail with a bare -1 while the
 * kb served a conforming 200, and there was no test that took the bytes the kb
 * actually returns and asserted the client accepts them. Hours went into
 * rebuilding images to answer a question one assertion settles.
 *
 * `why` receives the first failing term, so a rejection names itself instead of
 * arriving as "blast radius lookup failed". One malformed edge anywhere rejects
 * the whole payload, which is deliberate -- a partial graph is worse than none
 * for impact analysis -- but it has to say so. */
static int blast_edge_valid(const cJSON *edge, const char *identity_field)
{
   if (!cJSON_IsObject(edge))
      return 0;
   const cJSON *identity = cJSON_GetObjectItemCaseSensitive(edge, identity_field);
   const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(edge, "provenance");
   const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(edge, "confidence");
   const cJSON *project = cJSON_GetObjectItemCaseSensitive(edge, "project");
   const cJSON *generation = cJSON_GetObjectItemCaseSensitive(edge, "generation");
   const cJSON *freshness = cJSON_GetObjectItemCaseSensitive(edge, "freshness");
   return cJSON_IsString(identity) && identity->valuestring[0] && cJSON_IsString(provenance) &&
          provenance->valuestring[0] && cJSON_IsString(confidence) && confidence->valuestring[0] &&
          cJSON_IsString(project) && project->valuestring[0] && cJSON_IsNumber(generation) &&
          cJSON_IsString(freshness) && freshness->valuestring[0];
}

int kb_client_index_blast_edges_valid(const void *edges_v, const char *identity_field)
{
   const cJSON *edges = (const cJSON *)edges_v;
   if (!cJSON_IsArray(edges))
      return 0;
   const cJSON *edge = NULL;
   cJSON_ArrayForEach(edge, edges) if (!blast_edge_valid(edge, identity_field)) return 0;
   return 1;
}

int kb_client_index_blast_response_valid(const void *resp_v, char *why, size_t why_n)
{
   const cJSON *resp = (const cJSON *)resp_v;
   if (why && why_n)
      why[0] = '\0';
#define BLAST_REJECT(term)                                                                         \
   do                                                                                              \
   {                                                                                               \
      if (why && why_n)                                                                            \
         snprintf(why, why_n, "%s", (term));                                                       \
      return 0;                                                                                    \
   } while (0)

   if (!resp)
      BLAST_REJECT("no response");
   if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(resp, "error")))
      BLAST_REJECT("error field");

   const cJSON *project = cJSON_GetObjectItemCaseSensitive(resp, "project");
   const cJSON *generation = cJSON_GetObjectItemCaseSensitive(resp, "generation");
   const cJSON *freshness = cJSON_GetObjectItemCaseSensitive(resp, "freshness");
   const cJSON *resolved = cJSON_GetObjectItemCaseSensitive(resp, "resolved");

   if (!cJSON_IsString(project) || !project->valuestring[0])
      BLAST_REJECT("project");
   if (!cJSON_IsNumber(generation))
      BLAST_REJECT("generation");
   if (!cJSON_IsString(freshness) || !freshness->valuestring[0])
      BLAST_REJECT("freshness");
   if (!cJSON_IsTrue(resolved))
      BLAST_REJECT("resolved");
   if (!kb_client_index_blast_edges_valid(
           cJSON_GetObjectItemCaseSensitive(resp, "dependency_edges"), "identity"))
      BLAST_REJECT("dependency_edges");
   if (!kb_client_index_blast_edges_valid(cJSON_GetObjectItemCaseSensitive(resp, "dependent_edges"),
                                          "path"))
      BLAST_REJECT("dependent_edges");
#undef BLAST_REJECT
   return 1;
}
