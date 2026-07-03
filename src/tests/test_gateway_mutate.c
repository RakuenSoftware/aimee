/* test_gateway_mutate.c: the gateway-mutation decision + snapshot/replace/provenance
 * helpers (proposal §2.2/§2.3). should_apply classifies every apply/bypass case;
 * snapshot is an independent deep copy; replace installs cleanly; provenance is
 * mark-only-after-replace / clear-on-bypass. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gateway_mutate.h"

/* A clean single-user-turn array (no orphaned tool pairs). */
static cJSON *clean_messages(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", "hello world this is a longer message");
   cJSON_AddItemToArray(arr, m);
   return arr;
}

/* An OpenAI-shape array with an assistant tool_call and NO matching tool result
 * (an orphan message_history_repair must fix -> a structural violation). */
static cJSON *orphaned_messages(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *u = cJSON_CreateObject();
   cJSON_AddStringToObject(u, "role", "user");
   cJSON_AddStringToObject(u, "content", "do it");
   cJSON_AddItemToArray(arr, u);
   cJSON *a = cJSON_CreateObject();
   cJSON_AddStringToObject(a, "role", "assistant");
   cJSON *calls = cJSON_AddArrayToObject(a, "tool_calls");
   cJSON *c = cJSON_CreateObject();
   cJSON_AddStringToObject(c, "id", "call_1");
   cJSON_AddStringToObject(c, "type", "function");
   cJSON *fn = cJSON_AddObjectToObject(c, "function");
   cJSON_AddStringToObject(fn, "name", "f");
   cJSON_AddStringToObject(fn, "arguments", "{}");
   cJSON_AddItemToArray(calls, c);
   cJSON_AddItemToArray(arr, a);
   return arr;
}

/* Populate a REDUCED, mutated, net-shrink result over `messages` (ownership stays
 * with the caller-provided array; should_apply never frees it). */
static void reduced_result(reduce_result_t *r, cJSON *messages)
{
   memset(r, 0, sizeof(*r));
   r->messages = messages;
   r->mutated = 1;
   r->reason = REDUCE_REASON_REDUCED;
   r->baseline_tokens = 100;
   r->reduced_tokens = 50;
   r->removed_tokens = 50;
}

static void test_should_apply(void)
{
   /* apply: clean net-shrink reduction */
   {
      cJSON *m = clean_messages();
      reduce_result_t r;
      reduced_result(&r, m);
      assert(gw_should_apply(0, &r) == GW_BYPASS_NONE);
      cJSON_Delete(m);
   }

   /* reducer internal-error class maps 1:1 */
   {
      reduce_result_t r;
      memset(&r, 0, sizeof(r));
      r.error = REDUCE_ERR_ALLOC_FAILED;
      assert(gw_should_apply(1, &r) == GW_BYPASS_REDUCE_ALLOC_FAILED);
      r.error = REDUCE_ERR_PARSE_FAILED;
      assert(gw_should_apply(1, &r) == GW_BYPASS_REDUCE_PARSE_FAILED);
      r.error = REDUCE_ERR_INTERNAL_ASSERTION;
      assert(gw_should_apply(1, &r) == GW_BYPASS_REDUCE_INTERNAL_ASSERTION);
      r.error = REDUCE_ERR_FORMAT_UNSUPPORTED;
      assert(gw_should_apply(1, &r) == GW_BYPASS_REDUCE_FORMAT_UNSUPPORTED);
      /* rc!=0 with no classification, and the NULL-res case, both fall to internal */
      r.error = REDUCE_ERR_NONE;
      assert(gw_should_apply(1, &r) == GW_BYPASS_REDUCE_INTERNAL_ASSERTION);
      assert(gw_should_apply(1, NULL) == GW_BYPASS_REDUCE_INTERNAL_ASSERTION);
   }

   /* no-op cases: null array / not mutated / not REDUCED / not a net shrink */
   {
      reduce_result_t r;
      memset(&r, 0, sizeof(r));
      r.reason = REDUCE_REASON_REDUCED;
      r.mutated = 1; /* messages NULL */
      assert(gw_should_apply(0, &r) == GW_BYPASS_NO_OP);

      cJSON *m = clean_messages();
      reduced_result(&r, m);
      r.mutated = 0;
      assert(gw_should_apply(0, &r) == GW_BYPASS_NO_OP);
      reduced_result(&r, m);
      r.reason = REDUCE_REASON_MEASURED;
      assert(gw_should_apply(0, &r) == GW_BYPASS_NO_OP);
      reduced_result(&r, m);
      r.reduced_tokens = r.baseline_tokens; /* no shrink */
      assert(gw_should_apply(0, &r) == GW_BYPASS_NO_OP);
      cJSON_Delete(m);
   }

   /* structural violation: an orphaned tool pair -> repair reports > 0 */
   {
      cJSON *m = orphaned_messages();
      reduce_result_t r;
      reduced_result(&r, m);
      assert(gw_should_apply(0, &r) == GW_BYPASS_STRUCTURAL_VIOLATION);
      cJSON_Delete(m);
   }

   /* every reason has a stable label */
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_NONE), "none") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_SNAPSHOT_OOM), "snapshot_oom") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_CONSTRUCT_FAILED), "construct_failed") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_REPLACE_FAILED), "replace_failed") == 0);
}

static void test_snapshot_independence(void)
{
   cJSON *orig = clean_messages();
   assert(gw_snapshot_messages(NULL) == NULL);
   cJSON *snap = gw_snapshot_messages(orig);
   assert(snap != NULL);
   assert(cJSON_GetArraySize(snap) == 1);
   assert(gw_snapshot_token_count(snap) > 0);

   /* mutate the original: the snapshot is a fully independent deep copy */
   cJSON *extra = cJSON_CreateObject();
   cJSON_AddStringToObject(extra, "role", "user");
   cJSON_AddStringToObject(extra, "content", "second");
   cJSON_AddItemToArray(orig, extra);
   assert(cJSON_GetArraySize(orig) == 2);
   assert(cJSON_GetArraySize(snap) == 1); /* unchanged */

   /* editing a string in the original does not touch the snapshot */
   cJSON *first = cJSON_GetArrayItem(orig, 0);
   cJSON_ReplaceItemInObjectCaseSensitive(first, "content", cJSON_CreateString("mutated"));
   cJSON *snap_first = cJSON_GetArrayItem(snap, 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(snap_first, "content")),
                 "hello world this is a longer message") == 0);

   cJSON_Delete(orig);
   cJSON_Delete(snap);
}

static void test_replace(void)
{
   /* replace an existing key: the reduced array is installed, old one freed */
   cJSON *container = cJSON_CreateObject();
   cJSON_AddItemToObject(container, "messages", clean_messages());
   cJSON *reduced = cJSON_CreateArray();
   cJSON_AddItemToArray(reduced, cJSON_CreateString("x"));
   assert(gw_replace_messages(container, "messages", reduced) == 0);
   assert(cJSON_GetObjectItemCaseSensitive(container, "messages") == reduced);
   cJSON_Delete(container);

   /* add when the key is absent */
   cJSON *c2 = cJSON_CreateObject();
   cJSON *r2 = cJSON_CreateArray();
   assert(gw_replace_messages(c2, "messages", r2) == 0);
   assert(cJSON_GetObjectItemCaseSensitive(c2, "messages") == r2);
   cJSON_Delete(c2);

   /* guards fire independently, and a rejected replace leaves the container intact
    * and the reduced array caller-owned (we free it ourselves here). */
   assert(gw_replace_messages(NULL, "messages", NULL) != 0);
   cJSON *c3 = cJSON_CreateObject();
   cJSON_AddItemToObject(c3, "messages", clean_messages());
   cJSON *r3 = cJSON_CreateArray();
   assert(gw_replace_messages(c3, "messages", NULL) != 0); /* NULL reduced rejected */
   assert(gw_replace_messages(NULL, "messages", r3) != 0); /* NULL container rejected */
   assert(gw_replace_messages(c3, NULL, r3) != 0);         /* NULL key rejected */
   /* container still holds its original 1-message array; r3 was never installed */
   cJSON *still = cJSON_GetObjectItemCaseSensitive(c3, "messages");
   assert(still && cJSON_GetArraySize(still) == 1);
   cJSON_Delete(r3); /* caller still owns it after the rejected calls */
   cJSON_Delete(c3);
}

static void test_provenance(void)
{
   reduce_state_t st;
   memset(&st, 0, sizeof(st));
   assert(st.reduced == 0);
   gw_provenance_mark_reduced(&st);
   assert(st.reduced == 1);
   gw_provenance_clear(&st);
   assert(st.reduced == 0);
   /* NULL-safe */
   gw_provenance_mark_reduced(NULL);
   gw_provenance_clear(NULL);
}

int main(void)
{
   printf("gateway_mutate: ");
   test_should_apply();
   test_snapshot_independence();
   test_replace();
   test_provenance();
   printf("ok\n");
   return 0;
}
