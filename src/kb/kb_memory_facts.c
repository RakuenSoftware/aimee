/* kb_memory_facts.c: connection adapter for the Go memory-fact pipeline.
 *
 * Job policy, leasing, prompt construction, deterministic extraction, model
 * response parsing, grounding, relation canonicalisation, endpoint-kind
 * selection, and retry decisions live in server-go/modules/memory. This file
 * retains only the connections the C KB process owns: the curator provider,
 * the memory event bus, and the transactional DB2 fact-commit ABI. */
#include "kb_memory_facts.h"
#include "kb_module_stage_adapters.h"

#include "cJSON.h"
#include "kb_curator_llm.h"
#include "log.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/rel_types_store.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MF_ERRBUF      256
#define MF_LLM_OUT_CAP 8192

static const char *json_string(const cJSON *object, const char *name)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
   return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static int json_integer(const cJSON *object, const char *name, int *out)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
   if (!out || !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
       item->valuedouble != (double)item->valueint)
      return -1;
   *out = item->valueint;
   return 0;
}

static int json_int64(const cJSON *object, const char *name, int64_t *out)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
   if (!out || !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
       item->valuedouble < (double)INT64_MIN || item->valuedouble > (double)INT64_MAX ||
       trunc(item->valuedouble) != item->valuedouble)
      return -1;
   *out = (int64_t)item->valuedouble;
   return 0;
}

static int actor_from_json(const cJSON *object, fact_actor_t *out)
{
   const char *principal = json_string(object, "principal");
   const char *transport = json_string(object, "transport_identity");
   const char *role = json_string(object, "role");
   int rank = 0, authenticated = 0;
   if (!out || !principal || !principal[0] || !role || !role[0] ||
       json_integer(object, "rank", &rank) != 0 ||
       json_integer(object, "authenticated", &authenticated) != 0 ||
       rank < FACT_ACTOR_MODEL || rank > FACT_ACTOR_USER)
      return -1;
   memset(out, 0, sizeof(*out));
   if (strlen(principal) >= sizeof(out->principal) || strlen(role) >= sizeof(out->role) ||
       (transport && strlen(transport) >= sizeof(out->transport_identity)))
      return -1;
   memcpy(out->principal, principal, strlen(principal) + 1);
   memcpy(out->role, role, strlen(role) + 1);
   if (transport)
      memcpy(out->transport_identity, transport, strlen(transport) + 1);
   else
      memcpy(out->transport_identity, "internal", sizeof("internal"));
   out->rank = (fact_actor_rank_t)rank;
   out->authenticated = authenticated != 0;
   return 0;
}

static int commit_candidates(const cJSON *array)
{
   if (!array)
      return 0;
   if (!cJSON_IsArray(array))
      return -1;
   int committed = 0;
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, array)
   {
      const char *subject = json_string(item, "subject");
      const char *relation = json_string(item, "relation");
      const char *object = json_string(item, "object");
      const char *assertion_kind = json_string(item, "assertion_kind");
      const char *valid_from = json_string(item, "valid_from");
      const char *valid_until = json_string(item, "valid_until");
      const cJSON *actor_json = cJSON_GetObjectItemCaseSensitive(item, "actor");
      const cJSON *evidence_json = cJSON_GetObjectItemCaseSensitive(item, "evidence");
      int subject_kind = 0, object_kind = 0;
      fact_actor_t actor;
      if (!subject || !subject[0] || !relation || !relation[0] || !object || !object[0] ||
          !assertion_kind || !assertion_kind[0] || !cJSON_IsObject(actor_json) ||
          !cJSON_IsObject(evidence_json) || json_integer(item, "subject_kind", &subject_kind) != 0 ||
          json_integer(item, "object_kind", &object_kind) != 0 ||
          actor_from_json(actor_json, &actor) != 0)
         return -1;

      fact_evidence_input_t evidence = {
          .source_kind = json_string(evidence_json, "source_kind"),
          .source_id = json_string(evidence_json, "source_id"),
          .source_span = json_string(evidence_json, "source_span"),
          .evidence_hash = json_string(evidence_json, "evidence_hash"),
          .actor_principal = json_string(evidence_json, "actor_principal"),
          .observed_at = json_string(evidence_json, "observed_at"),
          .ingest_run_id = json_string(evidence_json, "ingest_run_id"),
          .stance = json_string(evidence_json, "stance"),
      };
      if (!evidence.source_kind || !evidence.source_id || !evidence.source_span ||
          !evidence.evidence_hash || !evidence.actor_principal || !evidence.observed_at ||
          !evidence.ingest_run_id || !evidence.stance)
         return -1;

      fact_gate_verdict_t verdict = db2_fact_commit_with_actor(
          subject, (memory_node_kind_t)subject_kind, relation, object,
          (memory_node_kind_t)object_kind, &actor, 1, &evidence, assertion_kind,
          valid_from ? valid_from : "", valid_until ? valid_until : "");
      if (verdict == FACT_GATE_ACCEPT || verdict == FACT_GATE_NOVEL)
         committed++;
   }
   return committed;
}

static int finish_job(int64_t job_id, int success, const char *reason)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return -1;
   cJSON_AddStringToObject(request, "operation", "memory-facts-finish");
   cJSON_AddNumberToObject(request, "id", (double)job_id);
   cJSON_AddBoolToObject(request, "success", success != 0);
   if (reason && reason[0])
      cJSON_AddStringToObject(request, "reason", reason);
   cJSON *response = kb_module_memory_data(request);
   cJSON_Delete(request);
   const cJSON *updated = response ? cJSON_GetObjectItemCaseSensitive(response, "updated") : NULL;
   int ok = cJSON_IsTrue(updated);
   cJSON_Delete(response);
   return ok ? 0 : -1;
}

static cJSON *parse_response(int64_t job_id, const char *provider_response)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "operation", "memory-facts-parse");
   cJSON_AddNumberToObject(request, "id", (double)job_id);
   cJSON_AddStringToObject(request, "content", provider_response ? provider_response : "");
   cJSON *response = kb_module_memory_data(request);
   cJSON_Delete(request);
   return response;
}

int kb_memory_facts_drain(int batch)
{
   if (batch <= 0)
      return 0;
   int processed = 0;
   for (int i = 0; i < batch; i++)
   {
      cJSON *claim = cJSON_CreateObject();
      if (!claim)
         break;
      cJSON_AddStringToObject(claim, "operation", "memory-facts-claim");
      cJSON *claim_response = kb_module_memory_data(claim);
      cJSON_Delete(claim);
      const cJSON *work = claim_response
                              ? cJSON_GetObjectItemCaseSensitive(claim_response, "fact_work")
                              : NULL;
      if (!cJSON_IsObject(work))
      {
         cJSON_Delete(claim_response);
         break;
      }

      int64_t job_id = 0, memory_id = 0;
      const char *system_prompt = json_string(work, "system_prompt");
      const char *content = json_string(work, "content");
      const cJSON *pattern = cJSON_GetObjectItemCaseSensitive(work, "candidates");
      if (json_int64(work, "job_id", &job_id) != 0 ||
          json_int64(work, "memory_id", &memory_id) != 0 || job_id <= 0 || memory_id <= 0 ||
          !system_prompt || !content || commit_candidates(pattern) < 0)
      {
         if (job_id > 0)
            (void)finish_job(job_id, 0, "invalid memory module work item");
         cJSON_Delete(claim_response);
         processed++;
         continue;
      }

      cJSON *provider_request = cJSON_CreateObject();
      if (provider_request)
         cJSON_AddStringToObject(provider_request, "content", content);
      char *request_json = provider_request ? cJSON_PrintUnformatted(provider_request) : NULL;
      cJSON_Delete(provider_request);
      if (!request_json)
      {
         (void)finish_job(job_id, 0, "could not encode curator request");
         cJSON_Delete(claim_response);
         processed++;
         continue;
      }

      db2_lease_release_idle();
      char err[MF_ERRBUF] = "";
      char *provider_response = kb_curator_llm_run(
          KB_CURATOR_STAGE_EXTRACT_DOCS, system_prompt, request_json, NULL, "", MF_LLM_OUT_CAP,
          err, sizeof(err));
      free(request_json);
      if (!provider_response)
      {
         (void)finish_job(job_id, 0, err[0] ? err : "llm run failed");
         cJSON_Delete(claim_response);
         processed++;
         continue;
      }

      cJSON *parsed = parse_response(job_id, provider_response);
      free(provider_response);
      const cJSON *model = parsed
                               ? cJSON_GetObjectItemCaseSensitive(parsed, "fact_candidates")
                               : NULL;
      int committed = commit_candidates(model);
      if (!parsed || committed < 0)
         (void)finish_job(job_id, 0, "memory module rejected provider response");
      else
      {
         (void)finish_job(job_id, 1, "");
         if (committed > 0)
            aimee_log(LOG_INFO, "kb.memory.facts", "memory %lld -> %d typed fact(s)",
                      (long long)memory_id, committed);
      }
      cJSON_Delete(parsed);
      cJSON_Delete(claim_response);
      processed++;
   }
   return processed;
}
