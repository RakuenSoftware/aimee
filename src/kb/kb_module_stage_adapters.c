#define _POSIX_C_SOURCE 200809L

#include "kb_module_stage_adapters.h"

#include "kb_curator_grounding.h"
#include "kb_identity.h"
#include "kb_mdl.h"
#include "kb_route_acl.h"
#include "aimee.h"
#include "cJSON.h"
#include "css_analyze.h"
#include "css_render_oracle.h"
#include "log.h"
#include "memory.h"
#include "vault_crypto.h"
#include "vault_kek_check.h"
#include "vault_mutation_budget.h"
#include "vault_reseal_receipt.h"
#include "vault_witness_checkpoint.h"
#include "vault_witness_export.h"
#include "vault_witness_merkle.h"
#include "vault_witness_record.h"
#include "vault_witness_signer.h"
#include "vault_witness_verify.h"

#include <aimee/audit/audit_worm_chain.h>
#include <aimee/audit/obs_bus.h>
#include <aimee/control-web/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/db2/client.h>
#include <aimee/db2/host_contracts.h>
#include <aimee/kb-synthesis/module_api.h>
#include <aimee/learning/learning.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include "modules/db2/support/db2_pii_classifier.h" /* the §7 PII gate provider seam */
#include <aimee/postgres/module_api.h>

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KB_MODULE_STAGE_DEADLINE_NS (500ULL * 1000000ULL)
#define KB_MODULE_EMBED_DEADLINE_NS (25ULL * 1000000000ULL)

static atomic_uint_fast64_t next_trace = 1;

static const aimee_db2_vault_crypto_provider_t vault_crypto_provider = {
    .aad_build_v2 = vault_aad_build_v2,
    .aad_build_v1_safe = vault_aad_build_v1_safe,
    .random = vault_crypto_random,
    .dek_wrap = vault_dek_wrap,
    .dek_unwrap = vault_dek_unwrap,
    .secret_encrypt = vault_secret_encrypt,
    .secret_decrypt = vault_secret_decrypt,
    .kek_check_wrap = vault_kek_check_wrap,
    .kek_check_verify = vault_kek_check_verify,
};

static int64_t vault_reseal_deadline_ms(uint32_t per_call_ms)
{
   vault_mutation_budget_t *budget = vault_mutation_budget_current();
   if (budget)
      return vault_mutation_budget_deadline_ms(budget, per_call_ms);
   struct timespec now;
   if (!per_call_ms || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return -1;
   int64_t current = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
   return current <= INT64_MAX - per_call_ms ? current + per_call_ms : -1;
}

static const aimee_db2_vault_reseal_provider_t vault_reseal_provider = {
    .deadline_ms = vault_reseal_deadline_ms,
    .operation_id_to_hex = vault_reseal_operation_id_to_hex,
    .operation_id_from_hex = vault_reseal_operation_id_from_hex,
    .receipt_decode = vault_reseal_receipt_decode,
    .receipt_digest = vault_reseal_receipt_digest,
};

static int vault_witness_checkpoint_verify_contract(const vault_witness_checkpoint_t *checkpoint,
                                                    const vault_witness_anchor_t *anchors,
                                                    size_t anchor_count)
{
   return (int)vault_witness_checkpoint_verify(checkpoint, anchors, anchor_count);
}

static int vault_witness_export_frame_contract(int kind, const uint8_t *payload, size_t payload_len,
                                               uint8_t *out, size_t cap, size_t *out_len)
{
   if (kind < VAULT_WITNESS_EXPORT_RECORD || kind > VAULT_WITNESS_EXPORT_SNAPSHOT)
      return -1;
   return vault_witness_export_frame((vault_witness_export_kind_t)kind, payload, payload_len, out,
                                     cap, out_len);
}

static int
vault_witness_verify_checkpoint_run_contract(const vault_witness_checkpoint_t *checkpoints,
                                             size_t count, size_t *gap_after_index)
{
   return (int)vault_witness_verify_checkpoint_run(checkpoints, count, gap_after_index);
}

static const aimee_db2_vault_witness_provider_t vault_witness_provider = {
    .checkpoint_digest = vault_witness_checkpoint_digest,
    .checkpoint_encode = vault_witness_checkpoint_encode,
    .checkpoint_sign = vault_witness_checkpoint_sign,
    .checkpoint_verify = vault_witness_checkpoint_verify_contract,
    .export_frame = vault_witness_export_frame_contract,
    .leaf_hash = vault_witness_leaf_hash,
    .merkle_root = vault_witness_merkle_root,
    .record_digest = vault_witness_record_digest,
    .record_encode = vault_witness_record_encode,
    .shard_key_hash = vault_witness_shard_key_hash,
    .signer_identity = vault_witness_signer_identity,
    .verify_checkpoint_run = vault_witness_verify_checkpoint_run_contract,
};

static int css_render_compare(const char *before_json, const char *after_json, int *before_valid,
                              int *after_valid, int *available, int *equivalent, int *diff_count)
{
   if (!before_valid || !after_valid || !available || !equivalent || !diff_count)
      return -1;
   css_render_snapshot_t *before = before_json ? css_render_snapshot_parse(before_json) : NULL;
   css_render_snapshot_t *after = after_json ? css_render_snapshot_parse(after_json) : NULL;
   css_render_result_t *result = css_render_oracle_compare(before, after);
   if (!result)
   {
      css_render_snapshot_free(before);
      css_render_snapshot_free(after);
      return -1;
   }
   *before_valid = before != NULL;
   *after_valid = after != NULL;
   *available = result->available;
   *equivalent = result->equivalent;
   *diff_count = result->diff_count;
   css_render_result_free(result);
   css_render_snapshot_free(before);
   css_render_snapshot_free(after);
   return 0;
}

_Static_assert((int)AIMEE_DB2_PRINCIPAL_NONE == (int)KB_PRIN_NONE, "DB2 principal NONE ABI drift");
_Static_assert((int)AIMEE_DB2_PRINCIPAL_OIDC == (int)KB_PRIN_OIDC, "DB2 principal OIDC ABI drift");
_Static_assert((int)AIMEE_DB2_PRINCIPAL_CERT == (int)KB_PRIN_CERT, "DB2 principal CERT ABI drift");
_Static_assert((int)AIMEE_DB2_PRINCIPAL_OWNER == (int)KB_PRIN_OWNER,
               "DB2 principal OWNER ABI drift");
_Static_assert((int)AIMEE_DB2_PRINCIPAL_HOST == (int)KB_PRIN_HOST, "DB2 principal HOST ABI drift");
_Static_assert(AIMEE_DB2_CSS_CLASS_TOKEN_MAX == CSS_CLASS_TOKEN_MAX,
               "DB2 CSS class-token ABI drift");
_Static_assert(AIMEE_DB2_VAULT_KEK_LEN == VAULT_KEK_LEN, "DB2 vault KEK ABI drift");
_Static_assert(AIMEE_DB2_VAULT_DEK_LEN == VAULT_DEK_LEN, "DB2 vault DEK ABI drift");
_Static_assert(AIMEE_DB2_VAULT_NONCE_LEN == VAULT_GCM_NONCE_LEN, "DB2 vault nonce ABI drift");
_Static_assert(AIMEE_DB2_VAULT_TAG_LEN == VAULT_GCM_TAG_LEN, "DB2 vault tag ABI drift");
_Static_assert(AIMEE_DB2_VAULT_WRAPPED_DEK_LEN == VAULT_WRAPPED_DEK_LEN,
               "DB2 vault wrapped-DEK ABI drift");
_Static_assert(AIMEE_DB2_VAULT_RESEAL_RECEIPT_LEN == VAULT_RESEAL_RECEIPT_V1_LEN,
               "DB2 vault reseal-receipt ABI drift");
_Static_assert(AIMEE_DB2_VAULT_RESEAL_OPERATION_LEN == VAULT_RESEAL_OPERATION_ID_LEN,
               "DB2 vault reseal-operation ABI drift");
_Static_assert(AIMEE_DB2_VAULT_RESEAL_OPERATION_HEX == VAULT_RESEAL_OPERATION_HEX_LEN,
               "DB2 vault reseal-operation hex ABI drift");

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int call_module_with_budget(uint32_t event_kind, uint32_t stage_id, const void *request,
                                   uint32_t request_len, void *response, uint32_t response_capacity,
                                   uint32_t *response_len, uint64_t budget_ns)
{
   uint64_t now = monotonic_ns();
   if (!now || budget_ns > UINT64_MAX - now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   return obs_bus_module_call(event_kind, stage_id, trace, now + budget_ns, request, request_len,
                              response, response_capacity, response_len, NULL,
                              NULL) == AIMEE_MODULE_CALL_OK
              ? 0
              : -1;
}

static int call_module(uint32_t event_kind, uint32_t stage_id, const void *request,
                       uint32_t request_len, void *response, uint32_t response_capacity,
                       uint32_t *response_len)
{
   return call_module_with_budget(event_kind, stage_id, request, request_len, response,
                                  response_capacity, response_len, KB_MODULE_STAGE_DEADLINE_NS);
}

static aimee_module_call_result_t
call_db2(void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
         uint64_t deadline_ns, const void *request_body, uint32_t request_len, void *response_body,
         uint32_t response_capacity, uint32_t *response_len, aimee_module_cancelled_fn cancelled,
         void *cancel_context)
{
   (void)context;
   return obs_bus_module_call(event_kind, stage_id, trace_id, deadline_ns, request_body,
                              request_len, response_body, response_capacity, response_len,
                              cancelled, cancel_context);
}

static int grounding_decide(aimee_kb_synthesis_claim_kind_t claim_kind, const char *const *claims,
                            uint32_t claim_count, const char *const *callees, uint32_t callee_count,
                            aimee_kb_synthesis_grounding_decision_t *decision)
{
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
   uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!decision || aimee_kb_synthesis_request_encode(claim_kind, claims, claim_count, callees,
                                                      callee_count, request, sizeof(request)) != 0)
      return -1;
   if (call_module(AIMEE_KB_SYNTHESIS_EVENT_GROUNDING, AIMEE_KB_SYNTHESIS_STAGE_GROUNDING, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_kb_synthesis_response_decode(response, response_len, decision);
}

static int control_web_authorize(const char *method, const char *path, int *allowed)
{
   uint8_t request[AIMEE_CONTROL_WEB_REQUEST_LEN];
   uint8_t response[AIMEE_CONTROL_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!allowed ||
       aimee_control_web_request_encode(AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, method, path,
                                        request, sizeof(request)) != 0 ||
       call_module(AIMEE_CONTROL_WEB_EVENT_AUTHORIZE, AIMEE_CONTROL_WEB_STAGE_AUTHORIZE, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_control_web_response_decode(response, response_len, allowed);
}

static int score_mdl(const char *candidate, const char *evidence, double *l_candidate,
                     double *l_residual, double *total)
{
   if (!l_candidate || !l_residual || !total)
      return -1;
   kb_mdl_score_t score = {0};
   if (kb_mdl_score(candidate, evidence, &score) != 0)
      return -1;
   *l_candidate = score.l_candidate;
   *l_residual = score.l_residual;
   *total = score.total;
   return 0;
}

static int check_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   uint8_t request[AIMEE_MEMORY_GATE_REQUEST_LEN], response[AIMEE_MEMORY_GATE_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_memory_fact_verdict_t result;
   if (!verdict)
      return -1;
   if (aimee_memory_gate_request_encode((uint32_t)head_kind, rel_type, (uint32_t)tail_kind, request,
                                        sizeof(request)) != 0)
   {
      *verdict = AIMEE_DB2_FACT_GATE_BADARG;
      return 0;
   }
   if (call_module(AIMEE_MEMORY_EVENT_WRITE, AIMEE_MEMORY_STAGE_WRITE, request, sizeof(request),
                   response, sizeof(response), &response_len) != 0 ||
       aimee_memory_gate_response_decode(response, response_len, &result) != 0)
      return -1;
   switch (result)
   {
   case AIMEE_MEMORY_FACT_ACCEPT:
      *verdict = AIMEE_DB2_FACT_GATE_ACCEPT;
      return 0;
   case AIMEE_MEMORY_FACT_REJECT_KIND:
      *verdict = AIMEE_DB2_FACT_GATE_REJECT_KIND;
      return 0;
   case AIMEE_MEMORY_FACT_NOVEL:
      *verdict = AIMEE_DB2_FACT_GATE_NOVEL;
      return 0;
   case AIMEE_MEMORY_FACT_BADARG:
      *verdict = AIMEE_DB2_FACT_GATE_BADARG;
      return 0;
   default:
      return -1;
   }
}

_Static_assert(sizeof(((aimee_db2_fact_candidate_t *)0)->subject) ==
                   AIMEE_MEMORY_TRIPLE_SUBJECT_MAX,
               "memory wire subject capacity must match the DB2 host contract");
_Static_assert(sizeof(((aimee_db2_fact_candidate_t *)0)->rel_type) ==
                   AIMEE_MEMORY_TRIPLE_REL_TYPE_MAX,
               "memory wire relation capacity must match the DB2 host contract");
_Static_assert(sizeof(((aimee_db2_fact_candidate_t *)0)->object) == AIMEE_MEMORY_TRIPLE_OBJECT_MAX,
               "memory wire object capacity must match the DB2 host contract");

static int extract_facts(const char *text, aimee_db2_fact_candidate_t *out, int max, int *count)
{
   if (!text || !out || max <= 0 || !count)
      return -1;
   size_t request_len = aimee_memory_extract_request_size(text);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;

   size_t response_cap = AIMEE_MEMORY_EXTRACT_RESPONSE_MAX(max);
   uint8_t *request = malloc(request_len);
   aimee_memory_triple_t *triples = calloc((size_t)max, sizeof(*triples));
   uint8_t *response = malloc(response_cap);
   uint32_t response_len = 0, found = 0;
   int rc = -1;
   if (request && triples && response && response_cap <= UINT32_MAX &&
       aimee_memory_extract_request_encode(text, (uint32_t)max, request, request_len) == 0 &&
       call_module(AIMEE_MEMORY_EVENT_EXTRACT_INDEX, AIMEE_MEMORY_STAGE_EXTRACT_INDEX, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &response_len) == 0 &&
       aimee_memory_extract_response_decode(response, response_len, triples, (uint32_t)max,
                                            &found) == 0)
   {
      rc = 0;
      for (uint32_t i = 0; i < found; ++i)
      {
         if (triples[i].subject_kind > INT_MAX || triples[i].object_kind > INT_MAX)
         {
            rc = -1;
            break;
         }
         memset(&out[i], 0, sizeof(out[i]));
         memcpy(out[i].subject, triples[i].subject, sizeof(out[i].subject));
         memcpy(out[i].rel_type, triples[i].rel_type, sizeof(out[i].rel_type));
         memcpy(out[i].object, triples[i].object, sizeof(out[i].object));
         out[i].subject_kind = (int)triples[i].subject_kind;
         out[i].object_kind = (int)triples[i].object_kind;
      }
      if (rc == 0)
         *count = (int)found;
   }
   free(request);
   free(triples);
   free(response);
   return rc;
}

/* The §7 PII recall gate, module-backed.
 *
 * The gate's only callers are in db2 (fact_recall, fact_ingest, rel_types_store)
 * and those run HERE, in the kb -- but the kb registered no provider, so
 * memory_pii_turn_requests_sensitive() and the sensitivity batch fell back to
 * the in-process cue list in pii_classifier_primitives.c. That is precisely the
 * silent fallback docs/modules/memory.md warns about: "a registered provider
 * that is authoritative and never falls back to the local implementation,
 * because a silent fallback lets a broken module look healthy."
 *
 * aimee-server registered these and calls RETRIEVE, but nothing in the server
 * invokes the gate -- the mirror image of the placement gap, with the capability
 * wired on one side and consumed on the other. Registering here puts the
 * decision on the module in the daemon that actually asks the question.
 *
 * Failure stays fail-closed by construction: a turn classifier that errors reads
 * as "did not ask for sensitive data" (withhold), and a sensitivity batch that
 * errors makes fact_recall abandon the candidates rather than inject them. */
static int kb_memory_pii_turn(const char *turn_text, int *requests_sensitive)
{
   if (!turn_text || !requests_sensitive)
      return -1;
   size_t request_len = aimee_memory_pii_request_size(turn_text);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_MEMORY_PII_RESPONSE_LEN];
   uint32_t response_len = 0;
   if (!request)
      return -1;
   int rc =
       aimee_memory_pii_request_encode(turn_text, request, request_len) == 0 &&
               call_module(AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, request,
                           (uint32_t)request_len, response, sizeof(response), &response_len) == 0
           ? aimee_memory_pii_response_decode(response, response_len, requests_sensitive)
           : -1;
   free(request);
   return rc;
}

/* db2's seam is int-based (db2_pii_classifier.h); the wire tiers and
 * rel_sensitivity_t share their numbering, asserted at the server's adapter. */
static int kb_memory_pii_sensitivity(const char *const *rel_types, int count, int *out)
{
   if (!rel_types || !out || count <= 0)
      return -1;
   size_t request_len = aimee_memory_sens_request_size(rel_types, count);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   size_t response_cap = AIMEE_MEMORY_SENS_RESPONSE_MAX(count);
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   aimee_memory_sensitivity_t *tiers = calloc((size_t)count, sizeof(*tiers));
   uint32_t response_len = 0;
   int rc = -1;
   if (request && response && tiers && response_cap <= UINT32_MAX &&
       aimee_memory_sens_request_encode(rel_types, count, request, request_len) == 0 &&
       call_module(AIMEE_MEMORY_EVENT_RETRIEVE, AIMEE_MEMORY_STAGE_RETRIEVE, request,
                   (uint32_t)request_len, response, (uint32_t)response_cap, &response_len) == 0 &&
       aimee_memory_sens_response_decode(response, response_len, tiers, count) == 0)
   {
      for (int i = 0; i < count; ++i)
         out[i] = (int)tiers[i];
      rc = 0;
   }
   free(request);
   free(response);
   free(tiers);
   return rc;
}

_Static_assert(AIMEE_DB2_FACT_ATTR_MAX == AIMEE_MEMORY_SCAN_ATTR_MAX,
               "memory wire attribute capacity must match the DB2 host contract");

static int scan_fact_turn(const char *text, int *is_retraction, int *has_attr,
                          char attr[AIMEE_DB2_FACT_ATTR_MAX])
{
   if (!text || !is_retraction || !has_attr || !attr)
      return -1;
   size_t request_len = aimee_memory_scan_request_size(text);
   if (!request_len || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY || request_len > UINT32_MAX)
      return -1;
   uint8_t *request = malloc(request_len);
   uint8_t response[AIMEE_MEMORY_SCAN_RESPONSE_MAX];
   uint32_t response_len = 0;
   if (!request)
      return -1;
   int rc = aimee_memory_scan_request_encode(text, request, request_len) == 0 &&
                    call_module(AIMEE_MEMORY_EVENT_EXTRACT_INDEX, AIMEE_MEMORY_STAGE_EXTRACT_INDEX,
                                request, (uint32_t)request_len, response, sizeof(response),
                                &response_len) == 0
                ? aimee_memory_scan_response_decode(response, response_len, is_retraction, has_attr,
                                                    attr, AIMEE_DB2_FACT_ATTR_MAX)
                : -1;
   free(request);
   return rc;
}

static int embed_command_is_http(const char *command)
{
   return command && (strncmp(command, "http://", strlen("http://")) == 0 ||
                      strncmp(command, "https://", strlen("https://")) == 0);
}

static int json_optional_flag(const cJSON *root, const char *name, int *value)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
   if (!item)
   {
      *value = 0;
      return 0;
   }
   if (!cJSON_IsBool(item))
      return -1;
   *value = cJSON_IsTrue(item) ? 1 : 0;
   return 0;
}

static int embed_over_module(const char *text, const char *command, int input_type, float *out,
                             int max_dim)
{
   if ((size_t)max_dim > (AIMEE_MODULE_MESSAGE_MAX_BODY - 512u) / 32u)
      return 0;
   cJSON *request_json = cJSON_CreateObject();
   if (!request_json || !cJSON_AddStringToObject(request_json, "base_url", command) ||
       !cJSON_AddStringToObject(request_json, "input_type",
                                input_type == AIMEE_DB2_EMBED_QUERY ? "query" : "document") ||
       !cJSON_AddStringToObject(request_json, "text", text) ||
       !cJSON_AddNumberToObject(request_json, "max_dim", max_dim))
   {
      cJSON_Delete(request_json);
      return 0;
   }
   char *request = cJSON_PrintUnformatted(request_json);
   cJSON_Delete(request_json);
   size_t request_len = request ? strlen(request) : 0;
   size_t response_cap = 512u + (size_t)max_dim * 32u;
   uint8_t *response = calloc(response_cap + 1u, 1u);
   uint32_t response_len = 0;
   int dim = 0;
   if (!request || request_len == 0 || request_len > AIMEE_MODULE_MESSAGE_MAX_BODY ||
       request_len > UINT32_MAX || !response || response_cap > UINT32_MAX ||
       call_module_with_budget(AIMEE_MEMORY_EVENT_EMBED, AIMEE_MEMORY_STAGE_EMBED, request,
                               (uint32_t)request_len, response, (uint32_t)response_cap,
                               &response_len, KB_MODULE_EMBED_DEADLINE_NS) != 0)
      goto done;

   cJSON *root = cJSON_ParseWithLength((const char *)response, response_len);
   const cJSON *json_dim = root ? cJSON_GetObjectItemCaseSensitive(root, "dim") : NULL;
   const cJSON *vector = root ? cJSON_GetObjectItemCaseSensitive(root, "vector") : NULL;
   const cJSON *error = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
   int unavailable = 0, unauthorized = 0, truncated = 0;
   if (!cJSON_IsObject(root) || !cJSON_IsNumber(json_dim) ||
       json_dim->valuedouble != (double)json_dim->valueint || json_dim->valueint <= 0 ||
       json_dim->valueint > max_dim || !cJSON_IsArray(vector) ||
       cJSON_GetArraySize(vector) != json_dim->valueint ||
       (error && (!cJSON_IsString(error) || (error->valuestring && error->valuestring[0]))) ||
       json_optional_flag(root, "unavailable", &unavailable) != 0 || unavailable ||
       json_optional_flag(root, "unauthorized", &unauthorized) != 0 || unauthorized ||
       json_optional_flag(root, "truncated", &truncated) != 0)
   {
      cJSON_Delete(root);
      goto done;
   }
   for (int i = 0; i < json_dim->valueint; ++i)
   {
      const cJSON *component = cJSON_GetArrayItem(vector, i);
      if (!cJSON_IsNumber(component) || !isfinite(component->valuedouble))
      {
         cJSON_Delete(root);
         goto done;
      }
      out[i] = (float)component->valuedouble;
      if (!isfinite(out[i]))
      {
         cJSON_Delete(root);
         goto done;
      }
   }
   dim = json_dim->valueint;
   if (truncated)
      aimee_log(LOG_WARN, "memory",
                "embedding truncated: module emitted more than max_dim %d; recall degraded",
                max_dim);
   cJSON_Delete(root);

done:
   free(request);
   free(response);
   return dim;
}

static int embed_text(const char *text, const char *command, int input_type, float *out,
                      int max_dim)
{
   if (!text || !command || !command[0] || !out || max_dim <= 0 ||
       (input_type != AIMEE_DB2_EMBED_DOCUMENT && input_type != AIMEE_DB2_EMBED_QUERY))
      return 0;
   if (!embed_command_is_http(command))
      return memory_embed_text(text, command, (embed_input_type_t)input_type, out, max_dim);
   return embed_over_module(text, command, input_type, out, max_dim);
}

static int identity_key(int kind, const char *issuer, const char *subject, int authenticated,
                        char *out, size_t cap)
{
   kb_principal_t principal;
   memset(&principal, 0, sizeof(principal));
   if (!issuer || !subject || !out || cap < 2 || authenticated != 1 ||
       strnlen(issuer, sizeof(principal.issuer)) == sizeof(principal.issuer) ||
       strnlen(subject, sizeof(principal.subject)) == sizeof(principal.subject))
      return -1;

   switch (kind)
   {
   case AIMEE_DB2_PRINCIPAL_OIDC:
      principal.kind = KB_PRIN_OIDC;
      break;
   case AIMEE_DB2_PRINCIPAL_CERT:
      principal.kind = KB_PRIN_CERT;
      break;
   case AIMEE_DB2_PRINCIPAL_OWNER:
      principal.kind = KB_PRIN_OWNER;
      break;
   case AIMEE_DB2_PRINCIPAL_HOST:
      principal.kind = KB_PRIN_HOST;
      break;
   default:
      return -1;
   }
   memcpy(principal.issuer, issuer, strlen(issuer) + 1);
   memcpy(principal.subject, subject, strlen(subject) + 1);
   principal.authenticated = 1;
   return kb_identity_key(&principal, out, cap);
}

int kb_module_postgres_health_probe(int *schema_ok, int *have_pg_trgm, int *kb_tables_ok)
{
   uint8_t request[AIMEE_POSTGRES_REQUEST_LEN];
   uint8_t response[AIMEE_POSTGRES_RESPONSE_LEN] = {0};
   uint32_t response_len = 0;
   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (aimee_postgres_health_request_encode(request, sizeof(request)) != 0 ||
       call_module(AIMEE_POSTGRES_EVENT_HEALTH, AIMEE_POSTGRES_STAGE_HEALTH, request,
                   sizeof(request), response, sizeof(response), &response_len) != 0)
      return -1;
   return aimee_postgres_health_response_decode(response, response_len, schema_ok, have_pg_trgm,
                                                kb_tables_ok);
}

int kb_module_db2_health_probe(int *schema_ok, int *have_pg_trgm, int *kb_tables_ok)
{
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   return aimee_db2_health_call(call_db2, NULL, trace, now + KB_MODULE_STAGE_DEADLINE_NS, schema_ok,
                                have_pg_trgm, kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK
              ? 0
              : -1;
}

/* Signal classification, for the router that runs HERE. The signal-capture
 * route is served by the KB -- that is where the learning tables live -- but
 * the classifier was registered only by the daemon, so every signal reaching
 * this process was refused with "classification unavailable" and nothing was
 * ever recorded. The stage is the same one the daemon calls; only the caller
 * differs. */
static int learning_classify(const char *signal, uint32_t *sink_mask)
{
   uint8_t request[AIMEE_LEARNING_REQUEST_LEN], response[AIMEE_LEARNING_RESPONSE_LEN];
   uint32_t response_len = 0;
   return aimee_learning_request_encode(signal, request, sizeof(request)) == 0 &&
                  call_module(AIMEE_LEARNING_EVENT_OBSERVE, AIMEE_LEARNING_STAGE_OBSERVE, request,
                              sizeof(request), response, sizeof(response), &response_len) == 0
              ? aimee_learning_response_decode(response, response_len, sink_mask)
              : -1;
}

void kb_module_stage_adapters_configure(void)
{
   aimee_db2_register_audit_hash_provider(audit_worm_row_hash);
   aimee_db2_register_mdl_score_provider(score_mdl);
   aimee_db2_register_fact_gate_provider(check_fact_gate);
   aimee_db2_register_fact_extract_provider(extract_facts);
   aimee_db2_register_fact_scan_provider(scan_fact_turn);
   memory_pii_register_turn_classifier(kb_memory_pii_turn);
   memory_pii_register_sensitivity_batch(kb_memory_pii_sensitivity);
   aimee_db2_register_embed_provider(embed_text);
   aimee_db2_register_identity_key_provider(identity_key);
   aimee_db2_register_css_render_compare_provider(css_render_compare);
   aimee_db2_register_css_analysis_providers(css_analyze, css_stylesheet_free,
                                             css_extract_class_tokens);
   aimee_db2_register_vault_crypto_provider(&vault_crypto_provider);
   aimee_db2_register_vault_reseal_provider(&vault_reseal_provider);
   aimee_db2_register_vault_witness_provider(&vault_witness_provider);
   kb_curator_grounding_register_provider(grounding_decide);
   kb_route_acl_register_authorization_provider(control_web_authorize);
   learning_router_register_signal_classifier(learning_classify);
}
