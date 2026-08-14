/* economizer_module_client.c: see economizer_module_client.h.
 *
 * Thin by design. Every decision the economizer makes now lives in the Go
 * module; this file builds a request, makes one bus call, and installs the
 * result. It deliberately contains no policy — a second opinion here would be a
 * rule living in two languages. */
#include "economizer_module_client.h"

#include "module_json_call.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ECON_AUX_WIRE_VERSION 1u
#define ECON_AUX_HEADER_LEN   12u
#define ECON_COMPACT_MAGIC    0x504d434au
#define ECON_RECALL_MAGIC     0x4c435254u
#define ECON_STATS_MAGIC      0x41545354u
#define ECON_STATS_LEN        72u

static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; i++)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static uint64_t get_u64(const uint8_t *p)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < 8; i++)
      value |= (uint64_t)p[i] << (i * 8u);
   return value;
}

static void put_u16(uint8_t *p, uint16_t value)
{
   p[0] = (uint8_t)value;
   p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; i++)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static uint8_t *call_bytes(uint32_t event_kind, uint32_t stage_id, const void *request,
                           size_t request_len, size_t response_capacity, uint32_t *response_len)
{
   if (!response_len || request_len > UINT32_MAX || response_capacity == 0 ||
       response_capacity > UINT32_MAX || !obs_bus_module_available(event_kind))
      return NULL;
   uint8_t *response = malloc(response_capacity);
   if (!response)
      return NULL;
   static const uint8_t empty_request;
   const void *body = request_len ? request : &empty_request;
   aimee_module_call_result_t rc = obs_bus_module_call(
       event_kind, stage_id, 0, aimee_module_call_deadline_ns(ECON_MODULE_CALL_TIMEOUT_MS), body,
       (uint32_t)request_len, response, (uint32_t)response_capacity, response_len, NULL, NULL);
   if (rc != AIMEE_MODULE_CALL_OK)
   {
      free(response);
      return NULL;
   }
   return response;
}

static int decode_aux(const uint8_t *response, size_t response_len, uint32_t magic,
                      uint16_t *result, const uint8_t **payload, uint32_t *payload_len)
{
   if (!response || response_len < ECON_AUX_HEADER_LEN || get_u32(response) != magic ||
       get_u16(response + 4) != ECON_AUX_WIRE_VERSION)
      return -1;
   uint32_t n = get_u32(response + 8);
   if (n != response_len - ECON_AUX_HEADER_LEN)
      return -1;
   if (result)
      *result = get_u16(response + 6);
   if (payload)
      *payload = response + ECON_AUX_HEADER_LEN;
   if (payload_len)
      *payload_len = n;
   return 0;
}

static int spill_dir_valid(const char *dir)
{
   if (!dir || dir[0] != '/' || dir[1] == '\0')
      return 0;
   const char *part = dir + 1;
   for (const char *p = part;; p++)
   {
      if (*p != '/' && *p != '\0')
         continue;
      size_t n = (size_t)(p - part);
      if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
         return 0;
      if (*p == '\0')
         return 1;
      part = p + 1;
   }
}

static void add_bool(cJSON *o, const char *k, int v)
{
   if (v)
      cJSON_AddBoolToObject(o, k, 1);
}

static void add_int(cJSON *o, const char *k, int v)
{
   if (v)
      cJSON_AddNumberToObject(o, k, v);
}

static char *dup_or_null(const char *s)
{
   if (!s || !s[0])
      return NULL;
   size_t n = strlen(s);
   char *c = malloc(n + 1);
   if (c)
      memcpy(c, s, n + 1);
   return c;
}

void econ_module_result_free(econ_module_result_t *out)
{
   if (!out)
      return;
   free(out->recall_hint);
   free(out->state);
   out->recall_hint = NULL;
   out->state = NULL;
}

int econ_module_reduce(const cJSON *messages, const char *system_prompt, econ_module_seam_t seam,
                       const econ_module_request_t *req, cJSON **reduced, econ_module_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (reduced)
      *reduced = NULL;
   if (!messages || !cJSON_IsArray((cJSON *)messages) || !req || !reduced)
      return 1;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return 1;

   /* The transcript is attached by REFERENCE and detached before the payload is
    * freed, so a whole message history is not deep-copied just to be printed. */
   cJSON_AddItemReferenceToObject(payload, "messages", (cJSON *)messages);
   if (system_prompt && system_prompt[0])
      cJSON_AddStringToObject(payload, "system_prompt", system_prompt);
   cJSON_AddStringToObject(payload, "seam",
                           seam == ECON_MODULE_SEAM_GATEWAY ? "gateway" : "delegate");

   add_bool(payload, "history_fold", req->history_fold);
   add_bool(payload, "compress", req->compress);
   add_bool(payload, "measure_only", req->measure_only);
   add_int(payload, "min_gain_tokens", req->min_gain_tokens);

   add_bool(payload, "freeze_guard_enabled", req->freeze_guard_enabled);
   add_int(payload, "freeze_guard_horizon", req->freeze_guard_horizon);
   if (req->priced)
   {
      cJSON *rates = cJSON_AddObjectToObject(payload, "rates");
      if (rates)
      {
         cJSON_AddBoolToObject(rates, "Priced", 1);
         cJSON_AddNumberToObject(rates, "InputCost", req->input_cost);
         cJSON_AddNumberToObject(rates, "WriteCost", req->write_cost);
         cJSON_AddNumberToObject(rates, "ReadCost", req->read_cost);
      }
   }

   add_bool(payload, "recall_enabled", req->recall_enabled);
   add_int(payload, "recall_ttl_turns", req->recall_ttl_turns);
   add_bool(payload, "recall_inject", req->recall_inject);

   add_int(payload, "retained_msgs", req->retained_msgs);
   add_int(payload, "min_fold_msgs", req->min_fold_msgs);
   add_int(payload, "excerpt_bytes", req->excerpt_bytes);
   add_bool(payload, "register_enabled", req->register_enabled);
   add_int(payload, "compact_head_bytes", req->compact_head_bytes);
   add_int(payload, "compact_tail_bytes", req->compact_tail_bytes);

   add_bool(payload, "closet_enabled", req->closet_enabled);
   add_int(payload, "closet_budget_bytes", req->closet_budget_bytes);
   add_int(payload, "closet_max_ratio_pct", req->closet_max_ratio_pct);
   if (req->closet_denylist && req->closet_denylist[0])
      cJSON_AddStringToObject(payload, "closet_denylist", req->closet_denylist);

   add_int(payload, "turn", req->turn);
   if (req->state && req->state[0])
      cJSON_AddStringToObject(payload, "state", req->state);

   /* Ask for the result code instead of discarding it. Passing NULL here made an
    * economizer that was never reached indistinguishable from one that ran and
    * declined: both surface as "no reduction", both are silent, and the only
    * visible difference is a token bill that does not fall. That ambiguity cost a
    * full night of bisecting a deployment which turned out to be fine. */
   aimee_module_call_result_t call_result = AIMEE_MODULE_CALL_OK;
   cJSON *reply =
       aimee_module_json_call(AIMEE_ECONOMIZER_EVENT_REDUCE, AIMEE_ECONOMIZER_STAGE_REDUCE, payload,
                              ECON_MODULE_CALL_MAX_BODY, ECON_MODULE_CALL_TIMEOUT_MS, &call_result);
   /* aimee_module_json_call TAKES OWNERSHIP and deletes payload on every path.
    * The messages child is a cJSON reference, so deleting its wrapper does not
    * delete the caller-owned transcript. Never touch payload after this call. */
   if (out)
      out->call_result = (int)call_result;
   if (!reply)
      return 1; /* unreachable / timed out -> caller keeps its original context */

   if (out)
   {
      const cJSON *v;
      if ((v = cJSON_GetObjectItemCaseSensitive(reply, "reason")) && cJSON_IsString(v))
         snprintf(out->reason, sizeof(out->reason), "%s", v->valuestring);
      /* Absent on the delegate seam, so a missing key leaves bypass empty rather
       * than defaulting to a verdict. */
      if ((v = cJSON_GetObjectItemCaseSensitive(reply, "bypass")) && cJSON_IsString(v))
         snprintf(out->bypass, sizeof(out->bypass), "%s", v->valuestring);
#define NUM(field, key)                                                                            \
   if ((v = cJSON_GetObjectItemCaseSensitive(reply, key)) && cJSON_IsNumber(v))                    \
   out->field = (int)v->valuedouble
      NUM(baseline_tokens, "baseline_tokens");
      NUM(reduced_tokens, "reduced_tokens");
      NUM(removed_tokens, "removed_tokens");
      NUM(foldable_tokens, "foldable_tokens");
      NUM(folded_msgs, "folded_msgs");
      NUM(retained_msgs, "retained_msgs");
      NUM(epochs, "epochs");
      NUM(recall_surfaced, "recall_surfaced");
#undef NUM
      out->reused_boundary = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)reply, "reused_boundary"));
      out->freeze_guarded = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)reply, "freeze_guarded"));
      out->closet_evicted = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)reply, "closet_evicted"));
      if ((v = cJSON_GetObjectItemCaseSensitive(reply, "recall_hint")) && cJSON_IsString(v))
         out->recall_hint = dup_or_null(v->valuestring);
      if ((v = cJSON_GetObjectItemCaseSensitive(reply, "state")) && cJSON_IsString(v))
         out->state = dup_or_null(v->valuestring);
   }

   /* An ABSENT messages field means "forward your original untouched" — the
    * module says so explicitly rather than echoing the transcript back, so the
    * common no-op path costs nothing and the caller's bytes are never replaced
    * by a re-serialized copy of themselves. */
   cJSON *body = cJSON_GetObjectItemCaseSensitive(reply, "messages");
   int mutated = cJSON_IsTrue(cJSON_GetObjectItem(reply, "mutated"));
   if (mutated && cJSON_IsArray(body))
   {
      cJSON *installed = cJSON_DetachItemFromObjectCaseSensitive(reply, "messages");
      if (installed)
      {
         *reduced = installed;
         if (out)
            out->mutated = 1;
      }
   }

   cJSON_Delete(reply);
   return 0;
}

int econ_module_json_compact(const void *input, size_t input_len, uint8_t **output,
                             size_t *output_len)
{
   if (output)
      *output = NULL;
   if (output_len)
      *output_len = 0;
   if (!input || input_len == 0 || input_len > ECON_MODULE_JSON_MAX_INPUT || !output || !output_len)
      return 1;

   uint32_t response_len = 0;
   uint8_t *response =
       call_bytes(AIMEE_ECONOMIZER_EVENT_JSON_COMPACT, AIMEE_ECONOMIZER_STAGE_JSON_COMPACT, input,
                  input_len, input_len + ECON_AUX_HEADER_LEN, &response_len);
   uint16_t result = 0;
   const uint8_t *payload = NULL;
   uint32_t payload_len = 0;
   if (!response ||
       decode_aux(response, response_len, ECON_COMPACT_MAGIC, &result, &payload, &payload_len) !=
           0 ||
       result != 0 || payload_len >= input_len)
   {
      free(response);
      return 1;
   }
   uint8_t *copy = malloc((size_t)payload_len + 1u);
   if (!copy)
   {
      free(response);
      return 1;
   }
   memcpy(copy, payload, payload_len);
   copy[payload_len] = 0;
   free(response);
   *output = copy;
   *output_len = payload_len;
   return 0;
}

int econ_module_tool_recall(const char *spill_dir, const char *ref, char **output, char *err,
                            size_t err_len)
{
   if (output)
      *output = NULL;
   if (err && err_len)
      err[0] = '\0';
   if (!spill_dir_valid(spill_dir) || !ref || !ref[0] || !output)
      goto invalid;
   size_t dir_len = strlen(spill_dir), ref_len = strlen(ref);
   if (dir_len > 4096u || ref_len > 64u || dir_len + ref_len > UINT32_MAX - 16u)
      goto invalid;
   size_t request_len = 16u + dir_len + ref_len;
   uint8_t *request = malloc(request_len);
   if (!request)
      goto unavailable;
   put_u32(request, ECON_RECALL_MAGIC);
   put_u16(request + 4, ECON_AUX_WIRE_VERSION);
   put_u16(request + 6, 0);
   put_u32(request + 8, (uint32_t)dir_len);
   put_u32(request + 12, (uint32_t)ref_len);
   memcpy(request + 16, spill_dir, dir_len);
   memcpy(request + 16 + dir_len, ref, ref_len);

   uint32_t response_len = 0;
   uint8_t *response =
       call_bytes(AIMEE_ECONOMIZER_EVENT_TOOL_RECALL, AIMEE_ECONOMIZER_STAGE_TOOL_RECALL, request,
                  request_len, ECON_MODULE_TOOL_OUTPUT_MAX + ECON_AUX_HEADER_LEN, &response_len);
   free(request);
   uint16_t result = 0;
   const uint8_t *payload = NULL;
   uint32_t payload_len = 0;
   if (!response ||
       decode_aux(response, response_len, ECON_RECALL_MAGIC, &result, &payload, &payload_len) != 0)
   {
      free(response);
      goto unavailable;
   }
   if (result != 0)
   {
      free(response);
      if (err && err_len)
         snprintf(err, err_len, "%s", result == 1 ? "invalid ref" : "spill expired");
      return 1;
   }
   char *copy = malloc((size_t)payload_len + 1u);
   if (!copy)
   {
      free(response);
      goto unavailable;
   }
   memcpy(copy, payload, payload_len);
   copy[payload_len] = '\0';
   free(response);
   *output = copy;
   return 0;

invalid:
   if (err && err_len)
      snprintf(err, err_len, "invalid ref");
   return 1;
unavailable:
   if (err && err_len)
      snprintf(err, err_len, "economizer unavailable");
   return 1;
}

int econ_module_tool_stats(econ_module_tool_totals_t *out)
{
   if (!out)
      return 1;
   memset(out, 0, sizeof(*out));
   uint32_t response_len = 0;
   uint8_t *response =
       call_bytes(AIMEE_ECONOMIZER_EVENT_TOOL_STATS, AIMEE_ECONOMIZER_STAGE_TOOL_STATS, NULL, 0,
                  ECON_STATS_LEN, &response_len);
   if (!response || response_len != ECON_STATS_LEN || get_u32(response) != ECON_STATS_MAGIC ||
       get_u16(response + 4) != ECON_AUX_WIRE_VERSION)
   {
      free(response);
      return 1;
   }
   long long *values[] = {&out->recognized,    &out->applied,        &out->applied_raw,
                          &out->applied_final, &out->family_test,    &out->family_diag,
                          &out->recovered,     &out->recovered_bytes};
   for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
      *values[i] = (long long)get_u64(response + 8u + i * 8u);
   free(response);
   out->saved_bytes = out->applied_raw - out->applied_final;
   out->net_saved_bytes = out->saved_bytes - out->recovered_bytes;
   return 0;
}

cJSON *econ_module_record_build(const cJSON *messages, int start_idx, int end_idx,
                                char **closet_out, int *closet_evict_out)
{
   if (closet_out)
      *closet_out = NULL;
   if (closet_evict_out)
      *closet_evict_out = ECON_MODULE_CLOSET_EVICT_NONE;
   if (!messages || !cJSON_IsArray((cJSON *)messages) || start_idx < 0 || end_idx < start_idx ||
       end_idx > cJSON_GetArraySize((cJSON *)messages))
      return NULL;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return NULL;
   cJSON_AddItemReferenceToObject(payload, "messages", (cJSON *)messages);
   cJSON_AddNumberToObject(payload, "start", start_idx);
   cJSON_AddNumberToObject(payload, "end", end_idx);
   cJSON *reply = aimee_module_json_call(
       AIMEE_ECONOMIZER_EVENT_RECORD_BUILD, AIMEE_ECONOMIZER_STAGE_RECORD_BUILD, payload,
       ECON_MODULE_CALL_MAX_BODY, ECON_MODULE_CALL_TIMEOUT_MS, NULL);
   /* The JSON-call helper has deleted payload. Its messages item was a borrowed
    * cJSON reference, so the original transcript remains caller-owned. */
   if (!reply)
      return NULL;

   cJSON *record = cJSON_GetObjectItemCaseSensitive(reply, "record");
   cJSON *files = record ? cJSON_GetObjectItemCaseSensitive(record, "files_modified") : NULL;
   cJSON *errors = record ? cJSON_GetObjectItemCaseSensitive(record, "errors_encountered") : NULL;
   cJSON *decisions = record ? cJSON_GetObjectItemCaseSensitive(record, "decisions_made") : NULL;
   if (!cJSON_IsObject(record) || !cJSON_IsArray(files) || !cJSON_IsArray(errors) ||
       !cJSON_IsArray(decisions))
   {
      cJSON_Delete(reply);
      return NULL;
   }

   cJSON *closet = cJSON_GetObjectItemCaseSensitive(reply, "closet");
   if (closet_out && cJSON_IsString(closet) && closet->valuestring[0])
   {
      *closet_out = dup_or_null(closet->valuestring);
      if (!*closet_out)
      {
         cJSON_Delete(reply);
         return NULL;
      }
   }
   if (closet_evict_out && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "closet_evicted")))
      *closet_evict_out = ECON_MODULE_CLOSET_EVICT_FAIL;

   record = cJSON_DetachItemFromObjectCaseSensitive(reply, "record");
   cJSON_Delete(reply);
   return record;
}
