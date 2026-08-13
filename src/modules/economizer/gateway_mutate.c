/* gateway_mutate.c: see gateway_mutate.h. Pure decision + snapshot/replace helpers
 * for the gateway-mutation path. */
#include "aimee.h" /* base typedefs (MAX_PATH_LEN etc.) required by agent_protocol.h */
#include "gateway_mutate.h"

#include <string.h>

#include "agent_protocol.h"  /* message_history_repair */
#include "session_compact.h" /* session_compact_estimate_tokens */

/* Is any ->next chain in this tree circular? Floyd's, read-only, no allocation,
 * applied at every level because a cycle nested inside one message is exactly as
 * fatal to a walker as one at the top.
 *
 * Deliberately here rather than reusing cJSON's internals: this is a POLICY
 * question (does the reduced view deserve to ship) and its answer has to be
 * distinguishable from an allocation failure, which a NULL out of
 * cJSON_Duplicate is not. */
int gw_messages_have_cycle(const cJSON *node)
{
   /* One pass settles this whole chain, so the descent below can walk it without
    * risking the very loop we are looking for. */
   const cJSON *slow = node;
   const cJSON *fast = node;
   while ((fast != NULL) && (fast->next != NULL))
   {
      slow = slow->next;
      fast = fast->next->next;
      if (slow == fast)
         return 1;
   }
   for (const cJSON *n = node; n != NULL; n = n->next)
   {
      if (gw_messages_have_cycle(n->child))
         return 1;
   }
   return 0;
}

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

gw_bypass_reason_t gw_should_apply(int reduce_rc, const gw_reduce_report_t *res)
{
   if (reduce_rc != 0 || !res)
      return GW_BYPASS_REDUCE_INTERNAL_ASSERTION;

   /* A genuine, applied reduction is the only thing worth mutating for: a new array,
    * marked mutated, with REASON_REDUCED. measure-only / already / skip / not-array
    * all land here as no-ops. */
   if (!res->messages || !res->mutated || res->reason != GW_REDUCE_REASON_REDUCED)
      return GW_BYPASS_NO_OP;

   /* Net shrink: a reduce that did not actually shrink is not worth the blast radius. */
   if (res->reduced_tokens >= res->baseline_tokens)
      return GW_BYPASS_NO_OP;

   /* A cyclic ->next in the reduced view is a structural violation, and naming it
    * one matters twice over.
    *
    * It USED TO BE the whole failure: cJSON_Duplicate's sibling walk was unbounded,
    * so this very probe -- the defence -- span here allocating a node per iteration
    * until aimee-server went 0.2 -> 6.9 GB in about ten seconds and was OOM-killed.
    * Duplicate now fails instead of hanging, which is what makes the check below
    * reachable at all.
    *
    * But Duplicate returns NULL for a cycle and for a genuine allocation failure
    * alike, and folding the two together would file every cycle under
    * "snapshot_oom" -- a stat that would send the next reader looking at memory
    * pressure instead of at whatever produced a malformed array. So detect it here,
    * before the probe, and report it as what it is. */
   if (gw_messages_have_cycle(res->messages))
      return GW_BYPASS_STRUCTURAL_VIOLATION;

   /* Structural check (defense in depth): the reduced view must have no orphaned
    * tool_use/tool_result pair. Run repair on a COPY so `res` is not mutated; any
    * repair the copy needed means the reduced view was structurally broken. */
   cJSON *probe = cJSON_Duplicate(res->messages, 1);
   if (!probe)
      return GW_BYPASS_SNAPSHOT_OOM; /* cannot verify -> never send un-verifiable */
   int repairs = message_history_repair(probe);
   cJSON_Delete(probe);
   /* != 0 (not > 0): any non-zero result — a repair count OR a hypothetical negative
    * error code — means the reduced view was not structurally clean, so bypass. */
   if (repairs != 0)
      return GW_BYPASS_STRUCTURAL_VIOLATION;

   return GW_BYPASS_NONE;
}

int gw_replace_messages(cJSON *container, const char *key, cJSON *reduced)
{
   if (!container || !key || !reduced)
      return 1;
   /* Replace item under key; cJSON_ReplaceItemInObjectCaseSensitive frees the old
    * array and installs `reduced` (taking ownership) ONLY on success. Its false
    * return (verified against the vendored cJSON: parent/child/item/replacement NULL)
    * does NOT delete the old item and does NOT install `reduced` — so on failure the
    * container is byte-intact and `reduced` stays caller-owned. If the key is absent,
    * add it (AddItemToObject likewise installs nothing on its OOM failure). */
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

void gw_provenance_mark_reduced(gw_provenance_t *st)
{
   if (st)
      st->reduced = 1;
}

void gw_provenance_clear(gw_provenance_t *st)
{
   if (st)
      st->reduced = 0;
}
