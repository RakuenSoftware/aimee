package memory

import (
	"context"
	"encoding/json"
	"math"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestGraphPathCredits(t *testing.T) {
	path := []GraphPathEntry{{Relation: "defines", Hop: 1}, {Relation: "calls", Hop: 2}, {Relation: "imports", Hop: 2}}
	for _, delta := range []float64{.1, -.1} {
		credits, err := graphPathCredits(delta, path)
		if err != nil {
			t.Fatal(err)
		}
		sum := 0.0
		for _, v := range credits {
			sum += v
			if v*delta <= 0 {
				t.Fatal(credits)
			}
		}
		if math.Abs(sum-delta) > 1e-12 || math.Abs(credits[0]) <= math.Abs(credits[2]) {
			t.Fatal(credits)
		}
	}
	if credits, err := graphPathCredits(.1, path[:1]); err != nil || credits[0] != .1 {
		t.Fatal(credits, err)
	}
	credits, err := graphPathCredits(.1, []GraphPathEntry{{Relation: "calls", Hop: 1}, {Relation: "calls", Hop: 2}})
	if err != nil || math.Abs(credits[0]-2*credits[1]) > 1e-12 {
		t.Fatal(credits, err)
	}
	for _, path := range [][]GraphPathEntry{nil, make([]GraphPathEntry, 33), {{Hop: 100000}}} {
		if _, err := graphPathCredits(.1, path); err == nil {
			t.Fatal("accepted empty, oversized or degenerate path")
		}
	}
	if _, err := graphPathCredits(math.NaN(), []GraphPathEntry{{}}); err == nil {
		t.Fatal("nonfinite delta")
	}
}

func exerciseGraphFeedbackReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	sql := func(query string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, query, args...); err != nil {
			t.Fatal(err)
		}
	}
	sql(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	sql(`RESET ROLE`)
	sql(`INSERT INTO projects(name,root,scanned_at,current_generation) VALUES('credit-project','/fixture/credit','now',2),('credit-hidden','/fixture/private','now',1)`)
	sql(`INSERT INTO code_embeddings(point_id,project,generation,node_key) VALUES(900000000001,'credit-project',2,'symbol:credit:visible'),(900000000002,'credit-project',1,'symbol:credit:stale'),(900000000003,'credit-hidden',1,'symbol:credit:hidden')`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	var id int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','graph-code-credit','Explanation without source query terms','project','credit-project') RETURNING id`).Scan(&id); err != nil {
		t.Fatal(err)
	}
	sql(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,'symbol:credit:visible'),($1,'credit:second')`, id)
	sql(`INSERT INTO entity_edges(source,relation,target,utility_score) VALUES('symbol:credit:visible','calls','credit:second',0),('symbol:credit:hidden','calls','hidden:target',0)`)
	sql(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
 VALUES('credit-replay-seed','fact.assert','test:graph','model',10,'open')`)
	sql(`INSERT INTO entity_edges(source,relation,target,edge_class,lifecycle_state,commit_id,ontology_version,confidence_class,authority_rank)
 VALUES('symbol:credit:visible','knows','semantic:target','semantic','candidate','credit-replay-seed',1,'C',10)`)
	s := *backend
	s.fusionEnabled = true
	req := DataRequest{Query: "explain handler.go", Project: "credit-project", Limit: 10, CodePointIDs: []int64{900000000001, 900000000002, 900000000003}}
	seeds, err := s.graphCodeSeeds(ctx, req, false)
	if err != nil || len(seeds) != 1 || seeds[0] != "symbol:credit:visible" {
		t.Fatal("generation/project admission", seeds, err)
	}
	records, err := s.fuseMemoryGraph(ctx, req, false, nil)
	if err != nil || len(records) != 1 || records[0].ID != id || records[0].graphScore <= 0 || records[0].codeProximity <= 0 {
		t.Fatal("code-only graph bridge/diagnostics", records, err)
	}
	parts := diagnosticFor(records[0], req.Query).Parts
	if parts.GraphScore != records[0].graphScore || parts.CodeProximity != records[0].codeProximity {
		t.Fatal(parts)
	}
	req.Query = "what did we decide"
	records, err = s.fuseMemoryGraph(ctx, req, false, nil)
	if err != nil || len(records) != 0 {
		t.Fatal("non-code query accepted code seed", records, err)
	}
	req.Query = "handler.go"
	req.Scope = Scope{Type: ScopeProject, Value: "credit-hidden"}
	req.IncludeAll = true
	seeds, err = s.graphCodeSeeds(ctx, req, true)
	if err != nil || len(seeds) != 1 || seeds[0] != "symbol:credit:hidden" {
		t.Fatal("exact scope bypass", seeds, err)
	}
	sql(`RESET ROLE`)
	sql(`UPDATE projects SET lifecycle_state='detached' WHERE name='credit-hidden'`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	seeds, err = s.graphCodeSeeds(ctx, req, true)
	if err != nil || len(seeds) != 0 {
		t.Fatal("detached project seed", seeds, err)
	}
	// Hidden evidence must not become an intermediate bridge into a visible hit.
	sql(`RESET ROLE`)
	sql(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance)
 SELECT id,'memory','memory:999999999999','supports' FROM entity_edges WHERE source='symbol:credit:visible' AND relation='calls'`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	visits, err := s.expandGraph(ctx, []string{"symbol:credit:visible"}, true, DataRequest{Project: "credit-project"}, false)
	if err != nil {
		t.Fatal(err)
	}
	for _, visit := range visits {
		if visit.Node == "credit:second" {
			t.Fatal("hidden evidence admitted as graph bridge", visits)
		}
	}
	sql(`RESET ROLE`)
	sql(`DELETE FROM fact_evidence WHERE source_id='memory:999999999999'`)
	sql(`SET LOCAL ROLE aimee_store_runtime`)
	handler := NewHandler(nil, WithDataStore(PlacementKB, &s))
	path := []GraphPathEntry{{Node: "symbol:credit:visible", Relation: "defines", Hop: 1}, {Node: "credit:second", Relation: "calls", Hop: 2}}
	args := map[string]any{"operation": "feedback-path", "path": path, "success": true, "scope_context": true, "project": "credit-project"}
	call := func(peer uint32) (map[string]any, bus.ModuleStatus) {
		t.Helper()
		raw, _ := json.Marshal(args)
		return invokeContextCommand(t, handler, peer, bus.CommandContext{}, "runtime", string(raw))
	}
	if _, status := call(200); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("public path injection", status)
	}
	if r, status := call(0); status != bus.ModuleStatusOK || r["updated"] != true {
		t.Fatal(r, status)
	}
	value := func(source, relation string) float64 {
		t.Helper()
		var v float64
		if err := tx.QueryRow(ctx, `SELECT utility_score FROM entity_edges WHERE source=$1 AND relation=$2`, source, relation).Scan(&v); err != nil {
			t.Fatal(err)
		}
		return v
	}
	if v := value("symbol:credit:visible", "calls"); math.Abs(v-.1) > 1e-9 {
		t.Fatal("incident credit not conserved", v)
	}
	if v := value("symbol:credit:visible", "knows"); v != 0 {
		t.Fatal("semantic mutation bypass", v)
	}
	args["path"] = []GraphPathEntry{{Node: "symbol:credit:hidden", Relation: "calls", Hop: 1}}
	if r, status := call(0); status != bus.ModuleStatusOK || r["updated"] != true {
		t.Fatal(r, status)
	}
	if v := value("symbol:credit:hidden", "calls"); v != 0 {
		t.Fatal("hidden code reinforced", v)
	}
	args["path"] = path
	args["success"] = false
	if r, status := call(0); status != bus.ModuleStatusOK || r["updated"] != true {
		t.Fatal(r, status)
	}
	if v := value("symbol:credit:visible", "calls"); math.Abs(v) > 1e-9 {
		t.Fatal("negative credit", v)
	}
	sql(`UPDATE entity_edges SET utility_score=5 WHERE source='symbol:credit:visible' AND relation='calls'`)
	args["success"] = true
	if _, status := call(0); status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	if v := value("symbol:credit:visible", "calls"); v != 5 {
		t.Fatal("utility clamp", v)
	}
	// The ordinary cited-record feedback route has the same semantic boundary.
	sql(`INSERT INTO entity_edges(source,relation,target) VALUES('graph-code-credit','calls','ordinary:target')`)
	sql(`INSERT INTO entity_edges(source,relation,target,edge_class,lifecycle_state,commit_id,ontology_version)
 VALUES('graph-code-credit','knows','ordinary:semantic','semantic','candidate','credit-replay-seed',1)`)
	if err := s.Feedback(ctx, Scope{Type: ScopeProject, Value: "credit-project"}, []int64{id}, true); err != nil {
		t.Fatal(err)
	}
	if v := value("graph-code-credit", "calls"); math.Abs(v-.1) > 1e-9 {
		t.Fatal(v)
	}
	if v := value("graph-code-credit", "knows"); v != 0 {
		t.Fatal("ordinary feedback bypassed semantic authority", v)
	}

}
