/* test_wfe_advance.c -- advance_request argument parsing and the event-bus-only
 * workflow decision seam. */
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

static int provider_calls;
static int provider_fails;
static wfe_advance_outcome_t provider_outcome;
static char provider_bound[WFE_ADVANCE_WI_LEN];

static int fake_event_bus_provider(const char *bound_wi, const wfe_advance_args_t *args,
                                   const char *actual_stage, const char *actual_state,
                                   const char *last_nonce, wfe_advance_outcome_t *outcome)
{
   provider_calls++;
   snprintf(provider_bound, sizeof(provider_bound), "%s", bound_wi ? bound_wi : "");
   assert(args != NULL);
   assert(strcmp(actual_stage ? actual_stage : "", "split") == 0);
   assert(strcmp(actual_state ? actual_state : "", "active") == 0);
   assert(strcmp(last_nonce ? last_nonce : "", "n-1") == 0);
   if (provider_fails)
      return -1;
   *outcome = provider_outcome;
   return 0;
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

static void test_decision_seam(void)
{
   wfe_advance_args_t a = mk("wi_1", "understand", NULL);
   wfe_advance_outcome_t outcome = WFE_ADV_BADARGS;

   wfe_advance_register_decision_provider(fake_event_bus_provider);
   provider_calls = provider_fails = 0;
   provider_outcome = WFE_ADV_REPLAY;
   assert(wfe_advance_decide("wi_1", &a, "split", "active", "n-1", &outcome) == 0);
   assert(provider_calls == 1 && outcome == WFE_ADV_REPLAY);
   assert(strcmp(provider_bound, "wi_1") == 0);

   /* Provider failure and malformed outcomes fail closed without local policy. */
   provider_fails = 1;
   assert(wfe_advance_decide("wi_1", &a, "split", "active", "n-1", &outcome) == -1);
   provider_fails = 0;
   provider_outcome = (wfe_advance_outcome_t)(WFE_ADV_BADARGS + 1);
   assert(wfe_advance_decide("wi_1", &a, "split", "active", "n-1", &outcome) == -1);

   wfe_advance_register_decision_provider(NULL);
   assert(wfe_advance_decide("wi_1", &a, "split", "active", "n-1", &outcome) == -1);
   assert(wfe_advance_decide("wi_1", NULL, "split", "active", "n-1", &outcome) == -1);
   assert(wfe_advance_decide("wi_1", &a, "split", "active", "n-1", NULL) == -1);
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
   test_decision_seam();
   test_tool_schema();
   printf("ok\n");
   return 0;
}
