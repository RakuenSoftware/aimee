package memory

import (
	"context"
	"fmt"
	"math"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

func TestGraphScoreParity(t *testing.T) {
	if graphGravity["defines"] != 1 || graphGravity["contains"] != .85 || graphGravity["imports"] != .30 {
		t.Fatal(graphGravity)
	}
	for class, want := range map[string]float64{"": 1, "A": 1, "a": 1, "B": .75, "C": .5, "Z": .5} {
		if graphConfidence(class) != want {
			t.Fatal(class)
		}
	}
	one := graphEdgeScore("defines", true, 3, 0, 0, 1, "")
	if one != 2 || graphEdgeScore("defines", true, 3, 0, 0, 2, "") != one/2 {
		t.Fatal("structure/hop decay")
	}
	plain := graphEdgeScore("calls", false, 0, 0, 0, 1, "")
	if graphEdgeScore("calls", false, 0, 5, 0, 1, "") <= plain || graphEdgeScore("calls", false, 0, 0, -.5, 1, "") != plain*.5 || graphEdgeScore("calls", false, 0, 0, 100, 1, "") != plain*3 {
		t.Fatal("observation/utility weighting")
	}
	co := graphEdgeScore("co_discussed", false, 0, 1, 0, 1, "")
	a, b, c := graphEdgeScore("works_for", false, 0, 1, 0, 1, "A"), graphEdgeScore("works_for", false, 0, 1, 0, 1, "B"), graphEdgeScore("works_for", false, 0, 1, 0, 1, "C")
	if !(a > b && b > c && a > co && c < co) || math.Abs(a/co-.8/.45) > 1e-9 {
		t.Fatal(a, b, c, co)
	}
	if graphEdgeScore("depends_on", false, 0, 1, 0, 1, "A") != graphEdgeScore("depends_on", false, 0, 1, 0, 1, "") {
		t.Fatal("explicit gravity lost")
	}
	now := time.Date(2026, 9, 18, 0, 0, 0, 0, time.UTC)
	if math.Abs(graphUtility(2, now.Add(-90*24*time.Hour).Format("2006-01-02 15:04:05"), now)-1) > 1e-9 {
		t.Fatal("utility half life")
	}
	if graphUtility(2, "1970-01-01 00:00:00", now) != 0 || graphUtility(2, "", now) != 2 || graphUtility(2, "invalid", now) != 2 || graphUtility(2, now.Add(time.Hour).Format(time.RFC3339), now) != 2 {
		t.Fatal("utility timestamp compatibility")
	}
	for _, query := range []string{"where is src/memory.c defined", "how does Foo::bar work", "bug at parser.c:42", "explain handler.go", "look at `parser.go`", "symbol:app:retry", "x->next"} {
		if !graphCodeQuery(query) {
			t.Fatal(query)
		}
	}
	for _, query := range []string{"what did we decide about deploys", "", "Alice works for Company"} {
		if graphCodeQuery(query) {
			t.Fatal(query)
		}
	}
}

func exerciseGraphFusionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	original := backend.fusionEnabled
	backend.fusionEnabled = true
	defer func() { backend.fusionEnabled = original }()
	seed := func(key, scope, entity string) Record {
		t.Helper()
		var r Record
		r.Scope = Scope{Type: ScopeProject, Value: scope}
		r.Key = key
		r.Content = "Relevant explanation without query terms"
		r.Tier = "L2"
		r.Kind = "fact"
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact',$1,$2,'project',$3) RETURNING id`, key, r.Content, scope).Scan(&r.ID); err != nil {
			t.Fatal(err)
		}
		if _, err := tx.Exec(ctx, `INSERT INTO memory_entities(memory_id,entity,role,weight) VALUES($1,$2,'actor',3)`, r.ID, entity); err != nil {
			t.Fatal(err)
		}
		return r
	}
	first := seed("graph-direct-seed", "graph-visible", "graph-shared-entity")
	bridge := seed("graph-direct-bridge", "graph-visible", "graph-shared-entity")
	hidden := seed("graph-hidden-bridge", "graph-hidden", "graph-shared-entity")
	second := seed("graph-second-hop", "graph-visible", "graph-neighbor")
	code := seed("graph-code-bridge", "graph-visible", "symbol:graph:retry")
	stale := seed("graph-stale-projection", "graph-visible", "graph-stale-node")
	candidate := seed("graph-unpromoted", "graph-visible", "graph-candidate-node")
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`INSERT INTO entity_edges(source,relation,target,weight) VALUES('graph-shared-entity','depends_on','graph-neighbor',2),
 ('graph-neighbor','defines','symbol:graph:retry',2),('graph-neighbor','related_to','graph-shared-entity',1)`)
	execSQL(`INSERT INTO entity_edges(source,relation,target,edge_origin,projection_generation_id) VALUES('graph-shared-entity','calls','graph-stale-node','code_projection',-1)`)
	execSQL(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
 VALUES('graph-replay-seed','fact.assert','test:graph','model',10,'open')`)
	execSQL(`INSERT INTO entity_edges(source,relation,target,edge_class,lifecycle_state,commit_id,ontology_version)
 VALUES('graph-shared-entity','works_for','graph-candidate-node','semantic','candidate','graph-replay-seed',1)`)
	req := DataRequest{Query: "graph-direct-seed", Project: "graph-visible", Limit: 64}
	records, err := backend.fuseMemoryGraph(ctx, req, false, []Record{first})
	if err != nil {
		t.Fatal(err)
	}
	has := func(rows []Record, id int64) bool {
		for _, r := range rows {
			if r.ID == id {
				return true
			}
		}
		return false
	}
	if !has(records, bridge.ID) || !has(records, second.ID) || has(records, hidden.ID) || has(records, code.ID) || has(records, stale.ID) || has(records, candidate.ID) {
		t.Fatal("bridge, scope or graph admission failed", records)
	}
	req.Query = "graph-direct-seed parser.go"
	records, err = backend.fuseMemoryGraph(ctx, req, false, []Record{first})
	if err != nil || !has(records, code.ID) {
		t.Fatal("code-shaped traversal lost", records, err)
	}
	req.Query = code.Key
	req.Scope = Scope{Type: ScopeProject, Value: "graph-visible"}
	// Exact scopes remain binding even if all-scope is also supplied internally.
	req.IncludeAll = true
	records, err = backend.fuseMemoryGraph(ctx, req, true, []Record{first})
	if err != nil || has(records, hidden.ID) {
		t.Fatal("exact scope lost", records, err)
	}
	visits, err := backend.expandGraph(ctx, []string{"symbol:graph:retry"}, false)
	if err != nil || len(visits) != 0 {
		t.Fatal("code seed gate lost", visits, err)
	}
	visits, err = backend.expandGraph(ctx, []string{"graph-shared-entity", "graph-shared-entity"}, true)
	if err != nil {
		t.Fatal(err)
	}
	seen := map[string]bool{}
	for _, v := range visits {
		if seen[v.Node] || v.Hop > 2 {
			t.Fatal("cycle or hop budget", visits)
		}
		seen[v.Node] = true
	}
	// Global node budget applies across all seeds, including an oversized caller.
	seeds := make([]string, 400)
	for i := range seeds {
		seeds[i] = fmt.Sprintf("bounded-seed-%d", i)
	}
	visits, err = backend.expandGraph(ctx, seeds, true)
	if err != nil || len(visits) != graphNodeBudget {
		t.Fatal("node budget", len(visits), err)
	}

	execSQL(`INSERT INTO entity_edges(source,relation,target,weight)
 SELECT 'budget-root-'||root,'related_to','budget-leaf-'||root||'-'||leaf,leaf
 FROM generate_series(1,8) root CROSS JOIN generate_series(1,64) leaf`)
	seeds = nil
	for i := 1; i <= 8; i++ {
		seeds = append(seeds, fmt.Sprintf("budget-root-%d", i))
	}
	visits, err = backend.expandGraph(ctx, seeds, true)
	if err != nil || len(visits) != graphNodeBudget {
		t.Fatal("fan-out escaped global budget", len(visits), err)
	}
	// The scope preference is applied before the result cap, even without a text
	// hit anchoring the project result in the first lane.
	global := seed("graph-global-bridge", "temporary-graph-scope", "graph-shared-entity")
	execSQL(`UPDATE memories SET scope_type='global',scope_value='_global' WHERE id=$1`, global.ID)
	records, err = backend.fuseMemoryGraph(ctx, DataRequest{Query: "graph-shared-entity", Project: "graph-visible", Limit: 1}, false, nil)
	if err != nil || len(records) != 1 || records[0].Scope.Value != "graph-visible" {
		t.Fatal("scope priority lost before limit", records, err)
	}
	// An off instance does not query the graph at all.
	backend.fusionEnabled = false
	records, err = backend.fuseMemoryGraph(ctx, req, false, []Record{first})
	if err != nil || len(records) != 1 || records[0].ID != first.ID {
		t.Fatal(records, err)
	}
}
