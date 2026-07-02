/* test_wfe_advance.c -- S2 sub-slice 3 pure core: the advance_request argument
 * parser + the interactive-driver CAS/replay decision. No engine/DB deps (the
 * executor integration is covered by test_wfe_advance_exec). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "wfe_advance.h"

static wfe_advance_args_t mk(const char *wi, const char *stage, const char *nonce)
{
   wfe_advance_args_t a;
   memset(&a, 0, sizeof a);
   if (wi)
      snprintf(a.work_item_id, sizeof a.work_item_id, "%s", wi);
   if (stage)
      snprintf(a.observed_stage, sizeof a.observed_stage, "%s", stage);
   if (nonce)
   {
      snprintf(a.nonce, sizeof a.nonce, "%s", nonce);
      a.have_nonce = 1;
   }
   return a;
}

static void test_parse(void)
{
   wfe_advance_args_t a;

   /* happy path, no nonce */
   assert(wfe_advance_parse_args("{\"work_item_id\":\"wi_abc\",\"observed_stage\":\"understand\"}",
                                 &a) == 0);
   assert(strcmp(a.work_item_id, "wi_abc") == 0);
   assert(strcmp(a.observed_stage, "understand") == 0);
   assert(a.have_nonce == 0);

   /* happy path, with nonce */
   assert(wfe_advance_parse_args(
              "{\"work_item_id\":\"wi_1\",\"observed_stage\":\"split\",\"nonce\":\"n-42\"}", &a) ==
          0);
   assert(a.have_nonce == 1 && strcmp(a.nonce, "n-42") == 0);

   /* missing required field -> fail closed */
   assert(wfe_advance_parse_args("{\"work_item_id\":\"wi_1\"}", &a) != 0);
   assert(wfe_advance_parse_args("{\"observed_stage\":\"split\"}", &a) != 0);

   /* out-of-charset id (path/JSON injection bytes) -> reject */
   assert(wfe_advance_parse_args("{\"work_item_id\":\"../etc\",\"observed_stage\":\"s\"}", &a) !=
          0);
   assert(wfe_advance_parse_args("{\"work_item_id\":\"wi_1\",\"observed_stage\":\"a b\"}", &a) !=
          0);
   /* present-but-malformed nonce -> reject the whole call */
   assert(wfe_advance_parse_args(
              "{\"work_item_id\":\"wi_1\",\"observed_stage\":\"s\",\"nonce\":\"a\\\"b\"}", &a) !=
          0);

   /* non-object / garbage / empty */
   assert(wfe_advance_parse_args("[]", &a) != 0);
   assert(wfe_advance_parse_args("not json", &a) != 0);
   assert(wfe_advance_parse_args("", &a) != 0);
   assert(wfe_advance_parse_args(NULL, &a) != 0);
   /* wrong type for a required field */
   assert(wfe_advance_parse_args("{\"work_item_id\":5,\"observed_stage\":\"s\"}", &a) != 0);
}

static void test_decide(void)
{
   wfe_advance_args_t a = mk("wi_1", "understand", NULL);

   /* clean advance: bound, active, stage matches */
   assert(wfe_advance_decide("wi_1", &a, "understand", "active", "") == WFE_ADV_OK);
   assert(wfe_advance_decide("wi_1", &a, "understand", "active", NULL) == WFE_ADV_OK);

   /* unbound, or bound to a different work-item (confused deputy) */
   assert(wfe_advance_decide("", &a, "understand", "active", "") == WFE_ADV_UNBOUND);
   assert(wfe_advance_decide(NULL, &a, "understand", "active", "") == WFE_ADV_UNBOUND);
   assert(wfe_advance_decide("wi_OTHER", &a, "understand", "active", "") == WFE_ADV_UNBOUND);

   /* stale CAS: observed stage no longer current */
   assert(wfe_advance_decide("wi_1", &a, "split", "active", "") == WFE_ADV_STALE);
   assert(wfe_advance_decide("wi_1", &a, "", "active", "") == WFE_ADV_STALE);

   /* terminal work-item cannot advance (no matching nonce) */
   assert(wfe_advance_decide("wi_1", &a, "understand", "accepted", "") == WFE_ADV_TERMINAL);
   assert(wfe_advance_decide("wi_1", &a, "understand", "rejected", "") == WFE_ADV_TERMINAL);
   assert(wfe_advance_decide("wi_1", &a, "understand", "abandoned", "") == WFE_ADV_TERMINAL);

   /* bad args */
   wfe_advance_args_t empty;
   memset(&empty, 0, sizeof empty);
   assert(wfe_advance_decide("wi_1", &empty, "understand", "active", "") == WFE_ADV_BADARGS);
   assert(wfe_advance_decide("wi_1", NULL, "understand", "active", "") == WFE_ADV_BADARGS);
}

static void test_replay_precedence(void)
{
   /* A retried turn: same nonce as the last applied advance. The stage has since
    * moved on (observed=understand, actual=split) AND the item may have gone
    * terminal -- REPLAY must win over both STALE and TERMINAL so the retry is an
    * idempotent no-op, never an error or a double-advance. */
   wfe_advance_args_t a = mk("wi_1", "understand", "n-7");

   assert(wfe_advance_decide("wi_1", &a, "split", "active", "n-7") == WFE_ADV_REPLAY);
   assert(wfe_advance_decide("wi_1", &a, "split", "accepted", "n-7") == WFE_ADV_REPLAY);

   /* a DIFFERENT nonce is a fresh advance, not a replay */
   assert(wfe_advance_decide("wi_1", &a, "understand", "active", "n-6") == WFE_ADV_OK);

   /* no last nonce recorded -> not a replay */
   assert(wfe_advance_decide("wi_1", &a, "understand", "active", "") == WFE_ADV_OK);

   /* args without a nonce never replay, even if a last nonce exists */
   wfe_advance_args_t no_nonce = mk("wi_1", "understand", NULL);
   assert(wfe_advance_decide("wi_1", &no_nonce, "understand", "active", "n-7") == WFE_ADV_OK);
}

static void test_tool_schema(void)
{
   assert(wfe_advance_tool_description() && wfe_advance_tool_description()[0]);
   assert(strcmp(wfe_advance_outcome_name(WFE_ADV_OK), "ok") == 0);
   assert(strcmp(wfe_advance_outcome_name(WFE_ADV_STALE), "stale") == 0);

   cJSON *p = wfe_advance_tool_params();
   assert(p);
   const cJSON *type = cJSON_GetObjectItemCaseSensitive(p, "type");
   assert(cJSON_IsString(type) && strcmp(type->valuestring, "object") == 0);
   const cJSON *props = cJSON_GetObjectItemCaseSensitive(p, "properties");
   assert(props && cJSON_GetObjectItemCaseSensitive(props, "work_item_id"));
   assert(cJSON_GetObjectItemCaseSensitive(props, "observed_stage"));
   assert(cJSON_GetObjectItemCaseSensitive(props, "nonce"));
   const cJSON *req = cJSON_GetObjectItemCaseSensitive(p, "required");
   assert(cJSON_IsArray(req) && cJSON_GetArraySize(req) == 2); /* nonce optional */
   cJSON_Delete(p);
}

int main(void)
{
   printf("wfe-advance: ");
   test_parse();
   test_decide();
   test_replay_precedence();
   test_tool_schema();
   printf("ok\n");
   return 0;
}
