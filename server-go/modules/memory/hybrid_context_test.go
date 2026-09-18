package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestHybridSymbolKey(t *testing.T) {
	for _, tc := range []struct{ project, symbol, want string }{
		{"app", "lookup", "symbol:app:lookup"},
		{"a:b/._~-", "x y%界", "symbol:a%3Ab/._~-:x%20y%25%E7%95%8C"},
		{strings.Repeat("p", 511), strings.Repeat("s", 511), "symbol:h:bd7be8a0e99e29fd18eeca95e06efb66"},
	} {
		if got := hybridSymbolKey(tc.project, tc.symbol); got != tc.want {
			t.Fatalf("key %q != %q", got, tc.want)
		}
	}
}
func TestHybridHostBoundary(t *testing.T) {
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"hybrid-context","query":"needle"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(placement, status)
		}
	}
}

func exerciseHybridReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT hybrid_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT hybrid_replay; RELEASE SAVEPOINT hybrid_replay`) }()
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true);
 INSERT INTO projects(name,root,scanned_at,lifecycle_state) VALUES('hybrid-fixture','/hybrid','now','current'),('hybrid-private','/private','now','current');`)
	content := "hybrid-replay " + strings.Repeat("界", 2000)
	const largeID int64 = 9007199254741507
	exec(`INSERT INTO memories(id,tier,kind,key,content,confidence,scope_type,scope_value) VALUES($1,'L2','decision','hybrid-replay',$2,.1,'project','hybrid-fixture')`, largeID, content)
	exec(`INSERT INTO memory_summaries(memory_id,scope,summary) VALUES($1,'headline','hybrid headline')`, largeID)
	exec(`INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value)
 SELECT 'L4','fact','hybrid-replay','global',1,'global','_global' FROM generate_series(1,12);
 INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value)
 SELECT 'L4','fact','hybrid-replay','private',1,'project','hybrid-private' FROM generate_series(1,12)`)
	var workspaceID int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value)
 VALUES('L2','fact','hybrid-replay','workspace',.1,'workspace','hybrid-workspace') RETURNING id`).Scan(&workspaceID); err != nil {
		t.Fatal(err)
	}

	var current, stale int64
	if err := tx.QueryRow(ctx, `INSERT INTO code_projection_generations(project,state) VALUES('hybrid-fixture','visible') RETURNING id`).Scan(&current); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO code_projection_generations(project,state) VALUES('hybrid-fixture','superseded') RETURNING id`).Scan(&stale); err != nil {
		t.Fatal(err)
	}
	seed := hybridSymbolKey("hybrid-fixture", "lookup")
	add := func(node, project, path, origin string, generation int64, weight int, edgeOrigin string, edgeGeneration int64) int64 {
		t.Helper()
		exec(`INSERT INTO entity_nodes(node_key,project,file_path,node_origin,last_seen_generation_id) VALUES($1,$2,$3,$4,$5)`, node, project, path, origin, generation)
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO entity_edges(source,relation,target,weight,structural_weight,edge_origin,projection_generation_id) VALUES($1,'relates_to',$2,$3,2,$4,$5) RETURNING id`, seed, node, weight, edgeOrigin, edgeGeneration).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	// More than the result cap of high-ranked ineligible neighbors cannot starve
	// the valid file. Duplicate neighbors occupy only one result slot.
	for i := 0; i < 30; i++ {
		add(fmt.Sprintf("hybrid:private:%d", i), "hybrid-private", "secret.go", "memory_extraction", 0, 100, "", 0)
	}
	add("hybrid:stale-node", "hybrid-fixture", "stale-node.go", "code_projection", stale, 100, "", 0)
	add("hybrid:stale-edge", "hybrid-fixture", "stale-edge.go", "memory_extraction", 0, 100, "code_projection", stale)
	add("hybrid:valid", "hybrid-fixture", "main.go", "code_projection", current, 5, "code_projection", current)
	add("hybrid:duplicate", "hybrid-fixture", "main.go", "memory_extraction", 0, 4, "", 0)
	edge := add("hybrid:semantic", "hybrid-fixture", "semantic.go", "memory_extraction", 0, 100, "", 0)
	exec(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
 VALUES('hybrid-replay-seed','fact.assert','test:hybrid','model',10,'open')`)
	exec(`UPDATE entity_edges SET edge_class='semantic',commit_id='hybrid-replay-seed',ontology_version=1 WHERE id=$1`, edge)
	edge = add("hybrid:evidence", "hybrid-fixture", "private-evidence.go", "memory_extraction", 0, 100, "", 0)
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id) SELECT $1,'memory','memory:'||id::text FROM memories WHERE key='hybrid-replay' AND scope_value='hybrid-private' LIMIT 1`, edge)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	// Port the former visible-search/LIKE native assertions through the actual
	// Go transaction boundary, with both distractor buckets larger than LIMIT.
	read := func(req DataRequest) []Record {
		t.Helper()
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		if status != bus.ModuleStatusOK {
			t.Fatal(req.Operation, status)
		}
		var response DataResponse
		if err := json.Unmarshal(raw, &response); err != nil {
			t.Fatal(err)
		}
		return response.Records
	}
	for _, operation := range []string{"visible-search", "query-records"} {
		req := DataRequest{Operation: operation, Mode: "like", Pattern: "hybrid-replay", Query: "hybrid-replay", Project: "hybrid-fixture", Workspace: "hybrid-workspace", Limit: 2}
		rows := read(req)
		if len(rows) != 2 || rows[0].ID != largeID || rows[1].ID != workspaceID {
			t.Fatalf("%s scope before limit: %+v", operation, rows)
		}
		req.Limit = 1
		if rows = read(req); len(rows) != 1 || rows[0].ID != largeID {
			t.Fatalf("%s local first: %+v", operation, rows)
		}
		req.IncludeAll, req.Limit = true, 64
		rows = read(req)
		if len(rows) != 26 || rows[0].ID != largeID || rows[1].ID != workspaceID || rows[len(rows)-1].Scope.Value != "hybrid-private" {
			t.Fatalf("%s all scope order: %+v", operation, rows)
		}
		req.IncludeAll, req.Project, req.Workspace = false, "", ""
		rows = read(req)
		if len(rows) != 12 {
			t.Fatalf("%s missing context count %d", operation, len(rows))
		}
		for _, row := range rows {
			if row.Scope.Type != ScopeGlobal {
				t.Fatal(row)
			}
		}
	}

	run := func(project string, all bool, symbol string) hybridMemoryResult {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"operation": "hybrid-context", "query": "hybrid-replay", "project": project, "include_all": all, "symbol": symbol, "scope_context": true})
		raw, _ := json.Marshal(runHostRuntime(t, handler, string(args)))
		var result hybridMemoryResult
		if err := json.Unmarshal(raw, &result); err != nil {
			t.Fatal(err)
		}
		return result
	}
	result := run("hybrid-fixture", false, "lookup")
	if result.Status != "ok" || len(result.Files) != 1 || result.Files[0].Path != "main.go" || result.Files[0].StructuralWeight != 2 {
		t.Fatalf("%+v", result.Files)
	}
	var why []hybridMemoryWhy
	if err := json.Unmarshal([]byte(result.WhyJSON), &why); err != nil {
		t.Fatal(err)
	}
	if len(why) != 5 || why[0].ID != largeID || why[0].Content != content || why[0].Headline != "hybrid headline" || strings.Contains(result.WhyJSON, `"private"`) {
		t.Fatalf("why rows=%d first=%+v", len(why), why[0])
	}
	if result = run("hybrid-fixture", false, ""); len(result.Files) != 0 {
		t.Fatal(result.Files)
	}
	result = run("", false, "lookup")
	if strings.Contains(result.WhyJSON, "private") || strings.Contains(result.WhyJSON, "hybrid headline") {
		t.Fatal(result.WhyJSON)
	}
	result = run("", true, "")
	if len(result.Files) != 0 {
		t.Fatal(result.Files)
	}
	// Owner configuration is authoritative even when the caller supplies a symbol.
	backend, _ := NewPostgresDataStore(runtimeRoleDB{evalQueryer{tx}, t}, PlacementKB)
	backend.(*postgresDataStore).fusionEnabled = false
	handler = NewHandler(nil, WithDataStore(PlacementKB, backend))
	if result = run("hybrid-fixture", false, "lookup"); len(result.Files) != 0 {
		t.Fatal(result.Files)
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
}
