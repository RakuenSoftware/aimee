/* wfe_advance.c -- see wfe_advance.h. Pure: no engine / DB / gateway deps. */
#include "wfe_advance.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* Bounded id-charset guard: [A-Za-z0-9_-], 1..cap-1 chars. The work-item id
 * (wi_<hex>), the stage (a validated YAML node id), and the nonce all live in this
 * charset; anything else is refused so a value that is later logged as JSON /
 * used as a routing key can never carry a separator or injection byte. */
static int id_ok(const char *s, size_t cap)
{
   if (!s || !s[0])
      return 0;
   size_t l = 0;
   for (const char *p = s; *p; p++, l++)
   {
      char c = *p;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-'))
         return 0;
   }
   return l < cap;
}

static const char *json_str(const cJSON *o, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

int wfe_advance_parse_args(const char *args_json, wfe_advance_args_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!args_json || !args_json[0])
      return -1;

   cJSON *j = cJSON_Parse(args_json);
   if (!j || !cJSON_IsObject(j))
   {
      cJSON_Delete(j);
      return -1;
   }

   int rc = -1;
   do
   {
      const char *wi = json_str(j, "work_item_id");
      const char *st = json_str(j, "observed_stage");
      const char *nc = json_str(j, "nonce"); /* optional */

      if (!id_ok(wi, sizeof out->work_item_id) || !id_ok(st, sizeof out->observed_stage))
         break; /* required fields missing / out of charset -> fail closed */
      /* a nonce, if present, must also be well-formed (it is later audit-logged). */
      if (nc && !id_ok(nc, sizeof out->nonce))
         break;

      snprintf(out->work_item_id, sizeof out->work_item_id, "%s", wi);
      snprintf(out->observed_stage, sizeof out->observed_stage, "%s", st);
      if (nc)
      {
         snprintf(out->nonce, sizeof out->nonce, "%s", nc);
         out->have_nonce = 1;
      }
      rc = 0;
   } while (0);

   cJSON_Delete(j);
   return rc;
}

int wfe_advance_state_is_terminal(const char *state)
{
   if (!state || !state[0])
      return 0;
   return strcmp(state, "accepted") == 0 || strcmp(state, "rejected") == 0 ||
          strcmp(state, "abandoned") == 0;
}

const char *wfe_advance_outcome_name(wfe_advance_outcome_t o)
{
   switch (o)
   {
   case WFE_ADV_OK:
      return "ok";
   case WFE_ADV_REPLAY:
      return "replay";
   case WFE_ADV_STALE:
      return "stale";
   case WFE_ADV_UNBOUND:
      return "unbound";
   case WFE_ADV_TERMINAL:
      return "terminal";
   case WFE_ADV_BADARGS:
      return "badargs";
   }
   return "badargs";
}

wfe_advance_outcome_t wfe_advance_decide(const char *bound_wi, const wfe_advance_args_t *a,
                                         const char *actual_stage, const char *actual_state,
                                         const char *last_nonce)
{
   if (!a || !a->work_item_id[0] || !a->observed_stage[0])
      return WFE_ADV_BADARGS;

   /* Unbound, or the caller's session is bound to a DIFFERENT work-item than the
    * one it is trying to advance (confused-deputy). Either way, refuse. */
   if (!bound_wi || !bound_wi[0] || strcmp(bound_wi, a->work_item_id) != 0)
      return WFE_ADV_UNBOUND;

   /* Idempotent replay MUST be decided before TERMINAL and STALE: a genuine retry
    * of an already-applied advance has both a stale observed_stage and (possibly)
    * a work-item that has since gone terminal, yet it must read as a no-op, not an
    * error. */
   if (a->have_nonce && last_nonce && last_nonce[0] && strcmp(a->nonce, last_nonce) == 0)
      return WFE_ADV_REPLAY;

   if (wfe_advance_state_is_terminal(actual_state))
      return WFE_ADV_TERMINAL;

   /* CAS: the primary's observed stage must still be the work-item's current stage
    * (a duplicate turn / a stage that already moved is rejected, not applied). */
   if (!actual_stage || strcmp(a->observed_stage, actual_stage) != 0)
      return WFE_ADV_STALE;

   return WFE_ADV_OK;
}

const char *wfe_advance_tool_description(void)
{
   return "Advance your bound work-item to the next workflow block. Call this ONLY "
          "after the current block's required artifact is complete; the engine "
          "enforces the review and delivery gates. Pass the work_item_id and the "
          "stage you observed as current.";
}

cJSON *wfe_advance_tool_params(void)
{
   cJSON *params = cJSON_CreateObject();
   if (!params)
      return NULL;
   cJSON_AddStringToObject(params, "type", "object");
   cJSON *props = cJSON_CreateObject();
   cJSON *p_wi = cJSON_CreateObject();
   cJSON_AddStringToObject(p_wi, "type", "string");
   cJSON_AddStringToObject(p_wi, "description",
                           "The id of the work-item this session is bound to.");
   cJSON_AddItemToObject(props, "work_item_id", p_wi);
   cJSON *p_st = cJSON_CreateObject();
   cJSON_AddStringToObject(p_st, "type", "string");
   cJSON_AddStringToObject(p_st, "description",
                           "The workflow stage you observed as current (compare-and-swap guard).");
   cJSON_AddItemToObject(props, "observed_stage", p_st);
   cJSON *p_nc = cJSON_CreateObject();
   cJSON_AddStringToObject(p_nc, "type", "string");
   cJSON_AddStringToObject(p_nc, "description",
                           "Optional idempotency token so a retried turn is not applied twice.");
   cJSON_AddItemToObject(props, "nonce", p_nc);
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("work_item_id"));
   cJSON_AddItemToArray(req, cJSON_CreateString("observed_stage"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}
