#include <aimee/core/turn_integrity.h>

#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ti_event_t events[16];
static int event_count;

static void capture(const ti_event_t *event, void *userdata)
{
   (void)userdata;
   assert(event_count < (int)(sizeof events / sizeof events[0]));
   events[event_count++] = *event;
}

static void test_lifecycle_and_json(void)
{
   ti_set_event_callback(capture, NULL);
   ti_turn_manifest_t turn;
   assert(ti_turn_manifest_init(&turn, "turn-17", "session-a", "user-a") == 0);
   assert(event_count == 1 && strcmp(events[0].event, "turn.created") == 0);

   ti_turn_snapshots_t snapshots = {0};
   snprintf(snapshots.configuration_id, sizeof snapshots.configuration_id, "cfg:4");
   snprintf(snapshots.toolset_id, sizeof snapshots.toolset_id, "tools:7");
   snprintf(snapshots.model_routing_id, sizeof snapshots.model_routing_id, "route:3");
   snprintf(snapshots.policy_revision, sizeof snapshots.policy_revision, "policy:9");
   snprintf(snapshots.context_manifest_id, sizeof snapshots.context_manifest_id, "ctx:12");
   assert(ti_turn_bind_snapshots(&turn, &snapshots) == 0);
   assert(ti_turn_bind_snapshots(&turn, &snapshots) == -1);
   assert(ti_turn_transition(&turn, TI_TURN_CONTEXTUALIZED, "assembled") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_CONTRACTED, "write") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_AUTHORIZED, "policy") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_EXECUTING, "tool") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_VERIFYING, "read-back") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_REVIEWING, "required") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_COMPLETED, "verified") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_FAILED, "late") == -1);

   cJSON *json = ti_turn_manifest_json(&turn);
   assert(json);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "state")), "completed") == 0);
   cJSON *snap = cJSON_GetObjectItem(json, "snapshots");
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(snap, "policy_revision")), "policy:9") ==
          0);
   cJSON_Delete(json);
}

static void test_read_only_and_terminal_paths(void)
{
   ti_turn_manifest_t turn;
   assert(ti_turn_manifest_init(&turn, "turn-read", "session-a", "user-a") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_CONTEXTUALIZED, NULL) == 0);
   assert(ti_turn_transition(&turn, TI_TURN_COMPLETED, "read-only") == 0);

   assert(ti_turn_manifest_init(&turn, "turn-block", "session-a", "user-a") == 0);
   assert(ti_turn_transition(&turn, TI_TURN_BLOCKED, "policy") == 0);
   assert(ti_turn_state_terminal(turn.state));
   assert(ti_turn_manifest_init(NULL, "x", NULL, NULL) == -1);
   assert(ti_turn_manifest_init(&turn, "", NULL, NULL) == -1);
}

static void test_scoped_knowledge_freshness(void)
{
   ti_knowledge_reset_for_test();
   assert(ti_knowledge_epoch_current("repository", "aimee") == 0);
   assert(ti_knowledge_epoch_advance("repository", "aimee", "commit changed") == 1);
   assert(ti_knowledge_epoch_current("repository", "aimee") == 1);
   assert(ti_knowledge_epoch_current("repository", "other") == 0);

   ti_knowledge_basis_t basis = {0};
   snprintf(basis.domain, sizeof basis.domain, "repository");
   snprintf(basis.scope_id, sizeof basis.scope_id, "aimee");
   basis.epoch = 1;
   assert(ti_knowledge_basis_freshness(&basis) == TI_FRESHNESS_CURRENT);
   assert(ti_knowledge_epoch_advance("repository", "aimee", "new commit") == 2);
   assert(ti_knowledge_basis_freshness(&basis) == TI_FRESHNESS_STALE);

   uint64_t previous = 99;
   assert(ti_session_knowledge_observe("session-k", 2, &previous) == TI_FRESHNESS_UNKNOWN);
   assert(previous == 0);
   assert(ti_session_knowledge_observe("session-k", 2, &previous) == TI_FRESHNESS_CURRENT);
   assert(previous == 2);
   assert(ti_session_knowledge_observe("session-k", 3, &previous) == TI_FRESHNESS_STALE);
   assert(previous == 2);
   assert(ti_session_knowledge_observe("other-session", 3, NULL) == TI_FRESHNESS_UNKNOWN);
   assert(ti_knowledge_epoch_advance("", "x", NULL) == 0);
}

static void test_effect_contract_shadow_lifecycle(void)
{
   event_count = 0;
   ti_effect_contract_t effect;
   assert(ti_effect_contract_init(&effect, "session-e", "write_file", "src/a.c",
                                  "{\"path\":\"src/a.c\",\"content\":\"secret\"}",
                                  TI_EFFECT_REVERSIBLE, TI_EFFECT_MODE_SHADOW) == 0);
   assert(strcmp(effect.tool, "write_file") == 0);
   assert(strstr(effect.arguments_digest, "secret") == NULL);
   assert(event_count == 1 && strcmp(events[0].event, "effect.proposed") == 0);
   assert(strstr(events[0].detail, "secret") == NULL);

   assert(ti_effect_contract_validate(&effect, "write_file", "src/a.c",
                                      "{\"path\":\"src/a.c\",\"content\":\"secret\"}",
                                      TI_EFFECT_REVERSIBLE) == 1);
   assert(ti_effect_contract_mark_executing(&effect) == 0);
   assert(ti_effect_contract_finish(&effect, TI_EFFECT_SUCCEEDED, "postcondition") == 0);
   assert(effect.state == TI_EFFECT_SUCCEEDED);
   assert(event_count == 4);
}

static void test_effect_contract_detects_drift(void)
{
   event_count = 0;
   ti_effect_contract_t effect;
   assert(ti_effect_contract_init(&effect, "session-e", "edit_file", "src/a.c", "{\"v\":1}",
                                  TI_EFFECT_REVERSIBLE, TI_EFFECT_MODE_SHADOW) == 0);
   assert(ti_effect_contract_validate(&effect, "edit_file", "src/b.c", "{\"v\":1}",
                                      TI_EFFECT_REVERSIBLE) == 0);
   assert(effect.matched == 0);
   assert(strcmp(events[1].event, "effect.mismatch") == 0);
   assert(ti_effect_contract_mark_executing(&effect) == 0);
   assert(ti_effect_contract_finish(&effect, TI_EFFECT_FAILED, "tool_error") == 0);
}

static void test_effect_postcondition_is_required_for_success(void)
{
   ti_effect_contract_t effect;
   assert(ti_effect_contract_init(&effect, "session-e", "write_file", "src/a.c", "{}",
                                  TI_EFFECT_REVERSIBLE, TI_EFFECT_MODE_ENFORCE) == 0);
   assert(ti_effect_contract_require_postcondition(&effect) == 0);
   assert(ti_effect_contract_validate(&effect, "write_file", "src/a.c", "{}",
                                      TI_EFFECT_REVERSIBLE) == 1);
   assert(ti_effect_contract_mark_executing(&effect) == 0);
   assert(ti_effect_contract_finish(&effect, TI_EFFECT_SUCCEEDED, "") == -1);
   assert(ti_effect_contract_record_postcondition(&effect, 1, "exact_readback") == 0);
   assert(ti_effect_contract_finish(&effect, TI_EFFECT_SUCCEEDED, "verified") == 0);

   assert(ti_effect_contract_init(&effect, "session-e", "write_file", "src/a.c", "{}",
                                  TI_EFFECT_REVERSIBLE, TI_EFFECT_MODE_ENFORCE) == 0);
   assert(ti_effect_contract_require_postcondition(&effect) == 0);
   assert(ti_effect_contract_validate(&effect, "write_file", "src/a.c", "{}",
                                      TI_EFFECT_REVERSIBLE) == 1);
   assert(ti_effect_contract_mark_executing(&effect) == 0);
   assert(ti_effect_contract_record_postcondition(&effect, 0, "exact_readback") == 0);
   assert(ti_effect_contract_finish(&effect, TI_EFFECT_SUCCEEDED, "") == -1);
   assert(ti_effect_contract_finish(&effect, TI_EFFECT_FAILED, "postcondition") == 0);
}

int main(void)
{
   test_lifecycle_and_json();
   test_read_only_and_terminal_paths();
   test_scoped_knowledge_freshness();
   test_effect_contract_shadow_lifecycle();
   test_effect_contract_detects_drift();
   test_effect_postcondition_is_required_for_success();
   ti_set_event_callback(NULL, NULL);
   puts("all turn_integrity tests passed");
   return 0;
}
