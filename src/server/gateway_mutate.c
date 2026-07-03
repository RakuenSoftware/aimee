/* gateway_mutate.c: see gateway_mutate.h. Pure decision + snapshot/replace helpers
 * for the gateway-mutation path. */
#include "aimee.h" /* base typedefs (MAX_PATH_LEN etc.) required by agent_protocol.h */
#include "gateway_mutate.h"

#include <string.h>

#include "agent_protocol.h"  /* message_history_repair */
#include "session_compact.h" /* session_compact_estimate_tokens */

const char *gw_bypass_reason_str(gw_bypass_reason_t reason)
{
   switch (reason)
   {
   case GW_BYPASS_NONE:
      return "none";
   case GW_BYPASS_REDUCE_ALLOC_FAILED:
      return "reduce_alloc_failed";
   case GW_BYPASS_REDUCE_PARSE_FAILED:
      return "reduce_parse_failed";
   case GW_BYPASS_REDUCE_INTERNAL_ASSERTION:
      return "reduce_internal_assertion";
   case GW_BYPASS_REDUCE_FORMAT_UNSUPPORTED:
      return "reduce_format_unsupported";
   case GW_BYPASS_NO_OP:
      return "no_op";
   case GW_BYPASS_STRUCTURAL_VIOLATION:
      return "structural_violation";
   case GW_BYPASS_SNAPSHOT_OOM:
      return "snapshot_oom";
   case GW_BYPASS_REPLACE_FAILED:
      return "replace_failed";
   case GW_BYPASS_CONSTRUCT_FAILED:
      return "construct_failed";
   }
   return "unknown";
}

cJSON *gw_snapshot_messages(const cJSON *messages)
{
   if (!messages)
      return NULL;
   /* cJSON_Duplicate(recurse=1) deep-copies every child + string, and frees all
    * partially-built sub-objects itself on any allocation failure, returning NULL. */
   return cJSON_Duplicate((cJSON *)messages, 1);
}

int gw_snapshot_token_count(cJSON *messages)
{
   if (!messages)
      return 0;
   return session_compact_estimate_tokens(messages);
}

/* Map the reducer's internal-error class to a gateway bypass reason. */
static gw_bypass_reason_t reduce_error_to_bypass(reduce_error_t e)
{
   switch (e)
   {
   case REDUCE_ERR_ALLOC_FAILED:
      return GW_BYPASS_REDUCE_ALLOC_FAILED;
   case REDUCE_ERR_PARSE_FAILED:
      return GW_BYPASS_REDUCE_PARSE_FAILED;
   case REDUCE_ERR_FORMAT_UNSUPPORTED:
      return GW_BYPASS_REDUCE_FORMAT_UNSUPPORTED;
   case REDUCE_ERR_INTERNAL_ASSERTION:
   case REDUCE_ERR_NONE:
   default:
      return GW_BYPASS_REDUCE_INTERNAL_ASSERTION;
   }
}

gw_bypass_reason_t gw_should_apply(int reduce_rc, const reduce_result_t *res)
{
   if (reduce_rc != 0 || !res)
      return reduce_error_to_bypass(res ? res->error : REDUCE_ERR_INTERNAL_ASSERTION);

   /* A genuine, applied reduction is the only thing worth mutating for: a new array,
    * marked mutated, with REASON_REDUCED. measure-only / already / skip / not-array
    * all land here as no-ops. */
   if (!res->messages || !res->mutated || res->reason != REDUCE_REASON_REDUCED)
      return GW_BYPASS_NO_OP;

   /* Net shrink: a reduce that did not actually shrink is not worth the blast radius. */
   if (res->reduced_tokens >= res->baseline_tokens)
      return GW_BYPASS_NO_OP;

   /* Structural check (defense in depth): the reduced view must have no orphaned
    * tool_use/tool_result pair. Run repair on a COPY so `res` is not mutated; any
    * repair the copy needed means the reduced view was structurally broken. */
   cJSON *probe = cJSON_Duplicate(res->messages, 1);
   if (!probe)
      return GW_BYPASS_SNAPSHOT_OOM; /* cannot verify -> never send un-verifiable */
   int repairs = message_history_repair(probe);
   cJSON_Delete(probe);
   if (repairs > 0)
      return GW_BYPASS_STRUCTURAL_VIOLATION;

   return GW_BYPASS_NONE;
}

int gw_replace_messages(cJSON *container, const char *key, cJSON *reduced)
{
   if (!container || !key || !reduced)
      return 1;
   /* Replace item under key; cJSON_ReplaceItemInObjectCaseSensitive detaches+frees
    * the old array and installs `reduced` (taking ownership) on success. If the key
    * is absent, add it. */
   if (cJSON_GetObjectItemCaseSensitive(container, key))
   {
      if (!cJSON_ReplaceItemInObjectCaseSensitive(container, key, reduced))
         return 1; /* container left intact; caller still owns `reduced` */
   }
   else
   {
      if (!cJSON_AddItemToObject(container, key, reduced))
         return 1;
   }
   return 0;
}

void gw_provenance_mark_reduced(reduce_state_t *st)
{
   if (st)
      st->reduced = 1;
}

void gw_provenance_clear(reduce_state_t *st)
{
   if (st)
      st->reduced = 0;
}
