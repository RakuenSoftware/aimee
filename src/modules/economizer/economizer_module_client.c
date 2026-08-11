/* economizer_module_client.c: see economizer_module_client.h.
 *
 * Thin by design. Every decision the economizer makes now lives in the Go
 * module; this file builds a request, makes one bus call, and installs the
 * result. It deliberately contains no policy — a second opinion here would be a
 * rule living in two languages. */
#include "economizer_module_client.h"

#include "module_json_call.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

   cJSON *reply =
       aimee_module_json_call(AIMEE_ECONOMIZER_EVENT_REDUCE, AIMEE_ECONOMIZER_STAGE_REDUCE, payload,
                              ECON_MODULE_CALL_MAX_BODY, ECON_MODULE_CALL_TIMEOUT_MS, NULL);
   /* Detach the borrowed transcript before the payload is freed, so the caller's
    * array survives regardless of how the call went. */
   cJSON_DetachItemFromObjectCaseSensitive(payload, "messages");
   cJSON_Delete(payload);

   if (!reply)
      return 1; /* unreachable / timed out -> caller keeps its original context */

   if (out)
   {
      const cJSON *v;
      if ((v = cJSON_GetObjectItemCaseSensitive(reply, "reason")) && cJSON_IsString(v))
         snprintf(out->reason, sizeof(out->reason), "%s", v->valuestring);
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
