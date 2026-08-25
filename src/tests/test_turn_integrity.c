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

int main(void)
{
   test_lifecycle_and_json();
   test_read_only_and_terminal_paths();
   test_scoped_knowledge_freshness();
   ti_set_event_callback(NULL, NULL);
   puts("all turn_integrity tests passed");
   return 0;
}
