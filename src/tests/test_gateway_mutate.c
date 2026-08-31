/* test_gateway_mutate.c: what C still owns on the gateway-mutation path
 * (proposal §2.2/§2.3). The apply/bypass decision lives in the Go economizer
 * module, so what is covered here is reading its verdict without ever treating
 * silence as consent; replace installs cleanly; provenance is
 * mark-only-after-replace / clear-on-bypass.
 *
 * The snapshot helpers are gone with the deep copy they served: the seam now
 * DETACHES the original array instead of duplicating it, so there is no copy to
 * prove independent of its source. */
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

/* The verdict now comes from the module, so what C still owns is refusing to
 * read silence as consent. Every path that is not a module saying "none" must
 * bypass, because the alternative is dispatching a payload nothing verified. */
static void test_module_bypass(void)
{
   /* A reached module that cleared the payload is the ONLY way to apply. */
   assert(strcmp(gw_module_bypass(0, "none"), "none") == 0);

   /* Its verdicts pass through verbatim, so telemetry keeps naming the reason. */
   assert(strcmp(gw_module_bypass(0, "structural_violation"), "structural_violation") == 0);
   assert(strcmp(gw_module_bypass(0, "no_op"), "no_op") == 0);

   /* Unreachable: rc says the call failed, so there is no verdict to trust. */
   assert(strcmp(gw_module_bypass(1, "none"), "reduce_internal_assertion") == 0);

   /* Reached but silent — an empty or absent verdict is not approval. This is
    * the delegate-seam shape too, which never sets the field at all. */
   assert(strcmp(gw_module_bypass(0, ""), "reduce_internal_assertion") == 0);
   assert(strcmp(gw_module_bypass(0, NULL), "reduce_internal_assertion") == 0);

   /* The labels stay stable because the module emits these exact strings and
    * dashboards key on them: a rename here silently desyncs the two sides. */
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_NONE), "none") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_SNAPSHOT_OOM), "snapshot_oom") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_CONSTRUCT_FAILED), "construct_failed") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_REPLACE_FAILED), "replace_failed") == 0);
   assert(strcmp(gw_bypass_reason_str(GW_BYPASS_STRUCTURAL_VIOLATION), "structural_violation") ==
          0);
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
   gw_provenance_t st;
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
   test_module_bypass();
   test_replace();
   test_provenance();
   printf("ok\n");
   return 0;
}
