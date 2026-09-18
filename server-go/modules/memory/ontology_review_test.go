package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestOntologyConsoleBoundary(t *testing.T) {
	operator := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "operator:test"}
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	for _, op := range []string{"ontology-dashboard", "ontology-review"} {
		args := `{"operation":"` + op + `"}`
		if _, status := invokeContextCommand(t, handler, 73, operator, "runtime", args); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if _, status := invokeContextCommand(t, NewHandler(nil, WithDataStore(PlacementServer, nil)), 0, operator, "runtime", args); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(status)
		}
	}
	for _, caller := range []bus.CommandContext{{}, {Authenticated: true, Principal: "user"}} {
		r, status := invokeContextCommand(t, handler, 0, caller, "runtime", `{"operation":"ontology-review","action":"approve","relation":"works_for","actor":{"rank":40}}`)
		if status != bus.ModuleStatusOK || r["http_status"] != float64(403) {
			t.Fatal(r, status)
		}
	}
	for _, name := range []string{"", "Upper", "has space", "has-dash", strings.Repeat("a", 64), "界"} {
		args, _ := json.Marshal(map[string]any{"operation": "ontology-review", "action": "approve", "relation": name})
		r, status := invokeContextCommand(t, handler, 0, operator, "runtime", string(args))
		if status != bus.ModuleStatusOK || r["http_status"] != float64(400) {
			t.Fatal(r, status)
		}
	}
	for _, args := range []string{`{"operation":"ontology-review","relation":"valid","action":"unknown"}`, `{"operation":"ontology-review","relation":"valid","action":"map"}`} {
		r, status := invokeContextCommand(t, handler, 0, operator, "runtime", args)
		if status != bus.ModuleStatusOK || r["http_status"] != float64(400) {
			t.Fatal(r, status)
		}
	}
	if !ontologyReviewName(strings.Repeat("a", 63)) || !ontologyReviewName("0_") {
		t.Fatal("legacy name boundary changed")
	}
}

func exerciseOntologyReviewReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT ontology_review_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT ontology_review_replay; RELEASE SAVEPOINT ontology_review_replay`) }()
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO ontology_evaluations(rel_type,occurrence_count,status,created_at) VALUES('onto_review',3,'pending','original'),('onto_map',2,'pending','original'),('onto_reject',1,'pending','original')`)
	exec(`INSERT INTO rel_types(rel_type,status) VALUES('onto_review','provisional'),('onto_map','provisional'),('onto_reject','provisional'),('onto_target','provisional')`)
	// Full text and IDs exceeding JavaScript's integer precision must survive the
	// Go command response and native transport without float64 reconstruction.
	name := strings.Repeat("界", 90)
	exec(`INSERT INTO entity_registry(canonical_id,kind,status) VALUES(9007199254740993,1,'active'),(9007199254740995,1,'merged')`)
	exec(`INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) VALUES($1,'onto-full-name',9007199254740993,1),('old name','onto-old-name',9007199254740995,1)`, name)
	exec(`INSERT INTO entity_merges(id,from_id,into_id) VALUES(9007199254740997,9007199254740995,9007199254740993)`)
	exec(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status) VALUES('onto-merge','entity.merge','fixture','operator',40,'open')`)
	exec(`INSERT INTO fact_graph_changes(commit_id,assertion_id,object_kind,object_key,action) VALUES('onto-merge',0,'entity_merge','9007199254740997','merge')`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	bound := *backend
	bound.settings = func() (map[string]any, error) {
		return map[string]any{"kb_typed_facts_promote_threshold": 0, "kb_typed_facts_auto_promote_enabled": true}, nil
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &bound))
	operator := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "operator:ontology", TransportIdentity: "verified:console"}
	call := func(args string, want int) map[string]json.RawMessage {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, operator, "runtime", args)
		if status != bus.ModuleStatusOK || r["http_status"] != float64(want) {
			t.Fatalf("%s: %v status=%v", args, r, status)
		}
		body, ok := r["json"].(string)
		if !ok {
			t.Fatal(r)
		}
		out := map[string]json.RawMessage{}
		if err := json.Unmarshal([]byte(body), &out); err != nil {
			t.Fatal(err)
		}
		return out
	}
	review := func(action, relation, target string, want int) {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"operation": "ontology-review", "action": action, "relation": relation, "target": target, "actor": map[string]any{"rank": 99, "principal": "forged"}})
		call(string(args), want)
	}
	dashboard := func() map[string]json.RawMessage { return call(`{"operation":"ontology-dashboard"}`, 200) }
	out := dashboard()
	if !strings.Contains(string(out["entities"]), "9007199254740993") || !strings.Contains(string(out["entities"]), name) || !strings.Contains(string(out["entity_merges"]), "9007199254740997") || !strings.Contains(string(out["entity_merges"]), "onto-merge") {
		t.Fatal(out)
	}
	var config struct {
		Threshold int  `json:"promote_threshold"`
		Auto      bool `json:"auto_promote"`
	}
	json.Unmarshal(out["config"], &config)
	if config.Threshold != 3 || !config.Auto {
		t.Fatal(config)
	}
	var candidates []struct {
		Relation string `json:"relation"`
		Count    int64  `json:"observations"`
		Ready    bool   `json:"ready"`
	}
	json.Unmarshal(out["promotion_candidates"], &candidates)
	if len(candidates) != 3 || candidates[0].Relation != "onto_review" || !candidates[0].Ready || candidates[1].Ready {
		t.Fatal(candidates)
	}
	review("approve", "onto_review", "", 200)
	review("approve", "onto_review", "", 200)
	review("map", "onto_map", "missing", 500)
	review("map", "onto_map", "onto_map", 500)
	review("map", "onto_map", "onto_target", 200)
	review("reject", "onto_reject", "", 200)
	review("approve", "unknown", "", 500)
	review("reject", "unknown", "", 500)
	var bad int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits c JOIN fact_graph_changes ch USING(commit_id) WHERE c.operation LIKE 'ontology.%' AND (c.actor_principal<>'operator:ontology' OR c.authority_rank<>40 OR c.actor_role<>'operator' OR c.status<>'applied' OR ch.assertion_id<>0 OR ch.object_kind<>'relation' OR ch.before_lifecycle<>'provisional/pending' OR ch.diff_detail<>'external graph transition' OR ch.existed_before<>1 OR ch.existed_after<>1 OR ch.action<>CASE c.operation WHEN 'ontology.approve' THEN 'promote' WHEN 'ontology.map' THEN 'map' ELSE 'reject' END OR ch.after_lifecycle<>CASE c.operation WHEN 'ontology.approve' THEN 'active/approved' WHEN 'ontology.map' THEN 'mapped' ELSE 'rejected' END)`).Scan(&bad); err != nil || bad != 0 {
		t.Fatal(bad, err)
	}
	for _, spec := range [][3]string{{"onto_review", "approved", "active"}, {"onto_map", "mapped", "mapped"}, {"onto_reject", "rejected", "rejected"}} {
		var a, b, date string
		if err := tx.QueryRow(ctx, `SELECT o.status,r.status,o.decided_at FROM ontology_evaluations o JOIN rel_types r USING(rel_type) WHERE rel_type=$1`, spec[0]).Scan(&a, &b, &date); err != nil || a != spec[1] || b != spec[2] || date == "" {
			t.Fatal(a, b, date, err)
		}
	}
	var target string
	if err := tx.QueryRow(ctx, `SELECT mapped_to FROM ontology_evaluations WHERE rel_type='onto_map'`).Scan(&target); err != nil || target != "onto_target" {
		t.Fatal(target, err)
	}
	exec(`RESET ROLE`)
	var seals int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits c JOIN fact_graph_changes ch USING(commit_id) JOIN kb_audit_outbox a ON a.detail='commit_id='||c.commit_id AND a.action=c.operation AND a.subject=ch.object_key WHERE c.operation LIKE 'ontology.%' AND a.actor_principal='operator:ontology' AND a.actor_role='operator' AND a.verdict='allow'`).Scan(&seals); err != nil || seals != 4 {
		t.Fatal(seals, err)
	}
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	// Re-observation through the real ingestion owner preserves rejection and
	// original creation time; duplicate evidence never increments the count.
	direct := bound
	direct.db = runtimeRoleTx{evalQueryer{tx}, t}
	candidate := FactCandidate{Subject: "Ontology observer", Relation: "Onto_Reject", Object: "literal", SubjectKind: NodePerson, ObjectKind: NodeOther, Actor: modelFactActor(), Evidence: FactEvidence{SourceKind: "observation", SourceID: "ontology-reobserve", Stance: "supports"}}
	for i := 0; i < 2; i++ {
		if _, _, err := direct.commitFactCandidate(ctx, candidate); err != nil {
			t.Fatal(err)
		}
	}
	var count int
	var status, created string
	if err := tx.QueryRow(ctx, `SELECT occurrence_count,status,created_at FROM ontology_evaluations WHERE rel_type='onto_reject'`).Scan(&count, &status, &created); err != nil || count != 2 || status != "rejected" || created != "original" {
		t.Fatal(count, status, created, err)
	}
	// A failure while sealing must roll back both ontology rows and the commit.
	exec(`RESET ROLE; CREATE FUNCTION pg_temp.ontology_fail_seal() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.operation LIKE 'ontology.%' AND NEW.status='applied' THEN RAISE EXCEPTION 'ontology late failure'; END IF; RETURN NEW; END $$; CREATE TRIGGER ontology_fail_seal BEFORE UPDATE ON fact_graph_commits FOR EACH ROW EXECUTE FUNCTION pg_temp.ontology_fail_seal(); SET LOCAL ROLE aimee_store_runtime`)
	var before, after int
	tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits WHERE operation LIKE 'ontology.%'`).Scan(&before)
	review("approve", "onto_reject", "", 500)
	tx.QueryRow(ctx, `SELECT count(*) FROM fact_graph_commits WHERE operation LIKE 'ontology.%'`).Scan(&after)
	if before != 4 || after != before {
		t.Fatal(before, after)
	}
	if err := tx.QueryRow(ctx, `SELECT status FROM rel_types WHERE rel_type='onto_reject'`).Scan(&status); err != nil || status != "rejected" {
		t.Fatal(status, err)
	}
	exec(`RESET ROLE; DROP TRIGGER ontology_fail_seal ON fact_graph_commits`)
	// Apply ordering before all three caps; the largest exact observation count
	// is retained alongside all preferred names, including names over 128 bytes.
	exec(`INSERT INTO ontology_evaluations(rel_type,occurrence_count) SELECT 'onto_pending_'||i,100 FROM generate_series(1,40)i`)
	exec(`UPDATE ontology_evaluations SET occurrence_count=9007199254740993 WHERE rel_type='onto_pending_9'`)
	exec(`INSERT INTO entity_registry(canonical_id,kind,status) SELECT 100000+i,1,'active' FROM generate_series(1,140)i`)
	exec(`INSERT INTO entity_merges(id,from_id,into_id) SELECT 100000+i,9007199254740995,9007199254740993 FROM generate_series(1,70)i`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	out = dashboard()
	for key, want := range map[string]string{"candidate_count": "32", "entity_count": "128", "entity_merge_count": "64"} {
		if string(out[key]) != want {
			t.Fatal(key, string(out[key]))
		}
	}
	json.Unmarshal(out["promotion_candidates"], &candidates)
	if candidates[0].Count != 9007199254740993 || candidates[0].Relation != "onto_pending_9" || candidates[1].Relation != "onto_pending_1" {
		t.Fatal(candidates[:2])
	}
	var entities []map[string]json.RawMessage
	json.Unmarshal(out["entities"], &entities)
	if string(entities[0]["canonical_id"]) != "9007199254740993" {
		t.Fatal(entities[0])
	}
	var forbidden bool
	if err := tx.QueryRow(ctx, `SELECT has_column_privilege(current_user,'rel_types','sensitivity','UPDATE') OR has_table_privilege(current_user,'entity_merges','UPDATE') OR has_column_privilege(current_user,'entity_merges','created_at','SELECT')`).Scan(&forbidden); err != nil || forbidden {
		t.Fatal(forbidden, err)
	}
	bound.settings = func() (map[string]any, error) { return nil, errors.New("unavailable") }
	call(`{"operation":"ontology-dashboard"}`, 503)
	bound.settings = func() (map[string]any, error) { return map[string]any{}, nil }
	exec(`RESET ROLE; REVOKE UPDATE(status) ON rel_types FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	review("approve", "onto_reject", "", 500)
	exec(`RESET ROLE; REVOKE SELECT(id) ON entity_merges FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call(`{"operation":"ontology-dashboard"}`, 503)
	t.Log(fmt.Sprintf("ontology review replay: %d sealed decisions", before))
}
