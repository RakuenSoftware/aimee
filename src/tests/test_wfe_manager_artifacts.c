/* test_wfe_manager_artifacts.c -- the typed schema contract for the
 * primary-as-manager artifacts (intent / packet-plan / review-verdict). Both the
 * S1 delegate executor and the S2 primary-agent executor must satisfy this same
 * contract, so it lives here as the single source of truth. Also pins the
 * forward-compat rule: unknown (S2-only) fields are tolerated and survive a
 * round-trip, so S2 needs no serialization migration. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wfe_manager_artifacts.h"

static cJSON *P(const char *json)
{
   cJSON *j = cJSON_Parse(json);
   assert(j);
   return j;
}

int main(void)
{
   printf("wfe-manager-artifacts: ");
   char err[256];

   /* ---- intent record ---- */
   cJSON *intent = P("{\"schema_version\":1,\"status\":\"unconfirmed\","
                     "\"summary\":\"add a logout button\","
                     "\"acceptance_criteria\":[\"button visible\",\"clears session\"]}");
   assert(wfe_intent_validate(intent, err, sizeof err) == 0);
   wfe_intent_status_t ist;
   assert(wfe_intent_status(intent, &ist) == 0 && ist == WFE_INTENT_UNCONFIRMED);
   cJSON_Delete(intent);

   /* wrong schema_version -> fail closed */
   cJSON *bad_ver = P("{\"schema_version\":2,\"status\":\"unconfirmed\","
                      "\"summary\":\"x\",\"acceptance_criteria\":[\"a\"]}");
   assert(wfe_intent_validate(bad_ver, err, sizeof err) != 0);
   cJSON_Delete(bad_ver);

   /* empty acceptance_criteria -> reject (must be >=1) */
   cJSON *no_ac = P("{\"schema_version\":1,\"status\":\"unconfirmed\","
                    "\"summary\":\"x\",\"acceptance_criteria\":[]}");
   assert(wfe_intent_validate(no_ac, err, sizeof err) != 0);
   cJSON_Delete(no_ac);

   /* bad status enum -> reject */
   cJSON *bad_status = P("{\"schema_version\":1,\"status\":\"maybe\","
                         "\"summary\":\"x\",\"acceptance_criteria\":[\"a\"]}");
   assert(wfe_intent_validate(bad_status, err, sizeof err) != 0);
   cJSON_Delete(bad_status);

   /* SELF-REFERENTIAL records -> reject: schema-valid garbage observed live,
    * where the scope delegate wrote a record about the record instead of the
    * task. Each is one of the three real-world shapes. */
   cJSON *self1 = P("{\"schema_version\":1,\"status\":\"confirmed\","
                    "\"summary\":\"Create INTENT RECORD for work item wi_2f9246\","
                    "\"acceptance_criteria\":[\"file exists\"]}");
   assert(wfe_intent_validate(self1, err, sizeof err) != 0);
   cJSON_Delete(self1);
   cJSON *self2 = P("{\"schema_version\":1,\"status\":\"confirmed\","
                    "\"summary\":\"Scope and structure work item as intent record\","
                    "\"acceptance_criteria\":[\"record committed\"]}");
   assert(wfe_intent_validate(self2, err, sizeof err) != 0);
   cJSON_Delete(self2);
   cJSON *self3 = P("{\"schema_version\":1,\"status\":\"confirmed\","
                    "\"summary\":\"Senior architecture review of 2f9246841f04f156 for integrity\","
                    "\"acceptance_criteria\":[\"review done\"]}");
   assert(wfe_intent_validate(self3, err, sizeof err) != 0); /* 16-hex id blob */
   cJSON_Delete(self3);
   /* a meta criterion alone also rejects */
   cJSON *self4 = P("{\"schema_version\":1,\"status\":\"confirmed\","
                    "\"summary\":\"discard first warmup instance per run\","
                    "\"acceptance_criteria\":[\"INTENT RECORD contains valid schema\"]}");
   assert(wfe_intent_validate(self4, err, sizeof err) != 0);
   cJSON_Delete(self4);
   /* a real task summary with ordinary hex-ish words still passes */
   cJSON *real = P("{\"schema_version\":1,\"status\":\"confirmed\","
                   "\"summary\":\"Implement warmup handling: discard first instance per run\","
                   "\"acceptance_criteria\":[\"first instance excluded from metrics\","
                   "\"warmup latency reported separately\"]}");
   assert(wfe_intent_validate(real, err, sizeof err) == 0);
   cJSON_Delete(real);

   /* ---- packet plan ---- */
   cJSON *packets = P("{\"schema_version\":1,\"packets\":[{\"packet_id\":\"p1\","
                      "\"summary\":\"impl the button\",\"target_blocks\":[\"implement\"],"
                      "\"dependencies\":[],\"acceptance_criteria\":[\"compiles\"]}]}");
   assert(wfe_packets_validate(packets, err, sizeof err) == 0);
   cJSON_Delete(packets);

   /* prose-only packet (missing structured fields) -> reject */
   cJSON *prose = P("{\"schema_version\":1,\"packets\":[{\"summary\":\"just do it\"}]}");
   assert(wfe_packets_validate(prose, err, sizeof err) != 0);
   cJSON_Delete(prose);

   /* empty packets -> reject */
   cJSON *no_pk = P("{\"schema_version\":1,\"packets\":[]}");
   assert(wfe_packets_validate(no_pk, err, sizeof err) != 0);
   cJSON_Delete(no_pk);

   /* ---- review verdict ---- */
   cJSON *pass = P("{\"schema_version\":1,\"verdict\":\"pass\","
                   "\"blocking_findings\":[],\"non_blocking\":[]}");
   assert(wfe_review_validate(pass, err, sizeof err) == 0);
   wfe_review_verdict_t rv;
   assert(wfe_review_verdict(pass, &rv) == 0 && rv == WFE_REVIEW_PASS);
   cJSON_Delete(pass);

   cJSON *changes = P("{\"schema_version\":1,\"verdict\":\"changes\",\"blocking_findings\":"
                      "[{\"block_id\":\"impl\",\"rule_id\":\"R1\",\"expected\":\"x\","
                      "\"observed\":\"y\",\"suggested_fix\":\"do z\"}],\"non_blocking\":[]}");
   assert(wfe_review_validate(changes, err, sizeof err) == 0);
   assert(wfe_review_verdict(changes, &rv) == 0 && rv == WFE_REVIEW_CHANGES);
   cJSON_Delete(changes);

   /* 'changes' with NO blocking findings -> reject (no delta to act on) */
   cJSON *empty_changes = P("{\"schema_version\":1,\"verdict\":\"changes\","
                            "\"blocking_findings\":[],\"non_blocking\":[]}");
   assert(wfe_review_validate(empty_changes, err, sizeof err) != 0);
   cJSON_Delete(empty_changes);

   /* a finding missing a required field (prose, not falsifiable) -> reject */
   cJSON *loose = P("{\"schema_version\":1,\"verdict\":\"changes\",\"blocking_findings\":"
                    "[{\"block_id\":\"impl\",\"observed\":\"looks off\"}],\"non_blocking\":[]}");
   assert(wfe_review_validate(loose, err, sizeof err) != 0);
   cJSON_Delete(loose);

   /* ---- forward-compat: S2-only fields are tolerated AND survive a round-trip
    * (so the S2 interactive slice needs no serialization migration) ---- */
   cJSON *s2 = P("{\"schema_version\":1,\"status\":\"confirmed\",\"summary\":\"x\","
                 "\"acceptance_criteria\":[\"a\"],"
                 "\"user_clarifications\":\"user said use a modal\","
                 "\"with_user_session_ref\":\"sess-123\","
                 "\"replay_anchor\":{\"turn\":7}}");
   assert(wfe_intent_validate(s2, err, sizeof err) == 0); /* extras tolerated */
   wfe_intent_status_t s2st;
   assert(wfe_intent_status(s2, &s2st) == 0 && s2st == WFE_INTENT_CONFIRMED);
   char *round = cJSON_PrintUnformatted(s2);
   assert(round && strstr(round, "with_user_session_ref") && strstr(round, "sess-123") &&
          strstr(round, "replay_anchor"));
   free(round);
   cJSON_Delete(s2);

   printf("ok\n");
   return 0;
}
