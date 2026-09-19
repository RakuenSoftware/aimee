package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestLearningMutationHostBoundary(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	for _, peer := range []uint32{1, 23, 200} {
		if _, status := invokeContextCommand(t, handler, peer, bus.CommandContext{}, "runtime", `{"operation":"learning-apply","sink":"reranker","action":{}}`); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(peer, status)
		}
	}
	for _, args := range []string{`{"sink":"supersede","action":{"old_memory_id":1.5,"new_content":"x"}}`, `{"sink":"workflow","action":{"project":"p"}}`, `{"sink":"other","action":{}}`, `{"sink":"reranker","action":null}`} {
		var input map[string]any
		json.Unmarshal([]byte(args), &input)
		input["operation"] = "learning-apply"
		body, _ := json.Marshal(input)
		if _, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "runtime", string(body)); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(args, status)
		}
	}
}

func exerciseLearningMutationReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact','learning-cited','source','project','learning-project') RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	execSQL(`INSERT INTO entity_edges(source,relation,target,weight,utility_score) VALUES('learning-cited','co_discussed','learning-target',1,0)`)
	run := func(sink string, action map[string]any, extra map[string]any) map[string]any {
		t.Helper()
		args := map[string]any{"operation": "learning-apply", "sink": sink, "action": action, "session_id": "learning-session"}
		for k, v := range extra {
			args[k] = v
		}
		raw, _ := json.Marshal(args)
		result, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "runtime", string(raw))
		if status != bus.ModuleStatusOK || result["status"] != "ok" {
			t.Fatal(result, status)
		}
		return result
	}
	run("reranker", map[string]any{"success": true}, map[string]any{"target_memory_id": id})
	var utility float64
	if err := tx.QueryRow(ctx, `SELECT utility_score FROM entity_edges WHERE source='learning-cited'`).Scan(&utility); err != nil || utility != 0.1 {
		t.Fatal(utility, err)
	}
	run("reranker", map[string]any{"success": false, "citation_ids": []any{-1, "bad", 1.5, id}}, nil)
	var corrections int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations WHERE memory_id=$1 AND relation='corrected_by' AND src_entity='learning-cited' AND dst_entity='learning-cited'`, id).Scan(&corrections); err != nil || corrections != 1 {
		t.Fatal(corrections, err)
	}
	if err := tx.QueryRow(ctx, `SELECT utility_score FROM entity_edges WHERE source='learning-cited'`).Scan(&utility); err != nil || utility != 0 {
		t.Fatal(utility, err)
	}
	// Explicit visibility excludes the source from both writes.
	run("reranker", map[string]any{"success": false, "citation_ids": []int64{id}}, map[string]any{"scope_context": true, "project": "another-project"})
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_relations WHERE memory_id=$1`, id).Scan(&corrections); err != nil || corrections != 1 {
		t.Fatal(corrections, err)
	}
	replacement := run("supersede", map[string]any{"old_memory_id": id, "new_content": "corrected source"}, nil)
	next := int64(replacement["id"].(float64))
	var interval, job bool
	var principal, scope string
	if err := tx.QueryRow(ctx, `SELECT o.valid_until=n.valid_from AND o.valid_until<>'',n.scope_value,a.actor_principal,
 EXISTS(SELECT 1 FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=n.id)
 FROM memories o JOIN memories n ON n.id=$2 JOIN memory_fact_actors a ON a.memory_id=n.id WHERE o.id=$1`, id, next).Scan(&interval, &scope, &principal, &job); err != nil || !interval || scope != "learning-project" || principal != "system:model-inference" || !job {
		t.Fatal(interval, scope, principal, job, err)
	}
	client := clientForHandler(t, handler)
	// Host observation plan -> canonical KB workflow write under the packaged
	// runtime role. Changed observations version the row; exact repeats retain identity.
	plan := runHostRuntime(t, handler, `{"operation":"workflow-plan","mode":"observe","command":"make test","cwd":"/dev/ObservedTeam/src","workspaces":["/dev/ObservedTeam"]}`)
	planned, err := json.Marshal(plan["request"])
	if err != nil {
		t.Fatal(err)
	}
	observed := runPublicCommand(t, client, "upsert_workflow", string(planned))
	if observed["status"] != "ok" {
		t.Fatal(observed)
	}
	var observedScope, observedSession, observedContent string
	var observedConfidence float64
	if err := tx.QueryRow(ctx, `SELECT scope_value,source_session,content,confidence FROM memories WHERE id=$1`, int64(observed["id"].(float64))).Scan(&observedScope, &observedSession, &observedContent, &observedConfidence); err != nil || observedScope != "ObservedTeam" || observedSession != "post_tool_update" || observedContent != "Test command: `make test`" || observedConfidence != 0.6 {
		t.Fatal(observedScope, observedSession, observedContent, observedConfidence, err)
	}

	first := runPublicCommand(t, client, "upsert_workflow", `{"workspace":"LearningTeam","signal_type":"PR","rule":"run tests","observed_confidence":0.6}`)
	if first["status"] != "ok" {
		t.Fatal(first)
	}
	workflowID := int64(first["id"].(float64))
	for i := 0; i < 3; i++ {
		again := runPublicCommand(t, client, "upsert_workflow", `{"workspace":"LearningTeam","signal_type":"PR","rule":"run all tests","observed_confidence":0.6}`)
		if again["status"] != "ok" || (i == 0 && again["id"] == first["id"]) || (i > 0 && again["id"] != float64(workflowID)) {
			t.Fatal(again, first)
		}
		workflowID = int64(again["id"].(float64))
	}
	var confidence, ceiling float64
	var key, session, provenance string
	var tagged bool
	if err := tx.QueryRow(ctx, `SELECT key,confidence,confidence_ceiling,source_session,provenance_category,
 EXISTS(SELECT 1 FROM memory_scopes WHERE memory_id=m.id AND scope_value='LearningTeam')
 FROM memories m WHERE id=$1`, workflowID).Scan(&key, &confidence, &ceiling, &session, &provenance, &tagged); err != nil || key != "workflow:learningteam:pr" || confidence != 0.8 || ceiling != 0.8 || session != "workflow_learning" || provenance != "agent_message" || !tagged {
		t.Fatal(key, confidence, ceiling, session, provenance, tagged, err)
	}
	r := run("workflow", map[string]any{"project": "LearningTeam", "signal_type": "PR", "rule": "review then test"}, nil)
	if r["id"] == float64(workflowID) {
		t.Fatal(r, first)
	}
	workflowID = int64(r["id"].(float64))
	if err := tx.QueryRow(ctx, `SELECT source_session FROM memories WHERE id=$1`, workflowID).Scan(&session); err != nil || session != "learning-session" {
		t.Fatal(session, err)
	}
	// Rejected content must not be resurrected by workflow learning.
	if r := runPublicCommand(t, client, "reject", fmt.Sprintf(`{"id":%d}`, workflowID)); r["status"] != "ok" {
		t.Fatal(r)
	}
	if r := runPublicCommand(t, client, "upsert_workflow", `{"workspace":"LearningTeam","signal_type":"PR","rule":"review then test"}`); r["status"] == "ok" {
		t.Fatal(r)
	}
}
