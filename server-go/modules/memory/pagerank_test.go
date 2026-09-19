package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"reflect"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestPageRankNativeKernelGolden(t *testing.T) {
	ids := []int64{9007199254740993, 2, 3, 4, 5}
	links := []MemoryLink{
		{SourceID: ids[0], TargetID: 2, Relation: "depends_on"}, {SourceID: ids[0], TargetID: 3, Relation: "depends_on"},
		{SourceID: ids[0], TargetID: 4, Relation: "depends_on"}, {SourceID: ids[0], TargetID: 2, Relation: "depends_on"},
		{SourceID: 2, TargetID: 3, Relation: "ignored"}, {SourceID: 5, TargetID: 5, Relation: "depends_on"},
		{SourceID: 5, TargetID: 99, Relation: "depends_on"},
	}
	// Captured by compiling memory_compute_pagerank_scores and its relation/index
	// helpers from 3d48beb23b^:src/modules/memory/memory_core_helpers.c. Storage and
	// the clock were fixture stubs; the kernel itself was unmodified.
	want := []float64{1.2, .72323020686968886, .41316970281580506, .41316970281580506, .1031091987619213}
	req := pageRankRequest{IDs: ids, Iterations: 8, Weight: 1.2, Relations: []string{"depends_on"}}
	got, edges, err := pageRankScores(context.Background(), ids, links, req)
	if err != nil || edges != 8 || len(got) != len(ids) {
		t.Fatal(got, edges, err)
	}
	for i, score := range got {
		if score.ID != ids[i] || math.Abs(score.Score-want[i]) > 1e-12 {
			t.Fatal(i, score, want[i])
		}
	}
	for _, ids := range [][]int64{nil, {1}, {1, 2, 3}} {
		scores, edges, err := pageRankScores(context.Background(), ids, nil, req)
		if err != nil || edges != 0 {
			t.Fatal(scores, edges, err)
		}
		if len(ids) < 2 && len(scores) != 0 {
			t.Fatal(scores)
		}
		for _, score := range scores {
			if score.Score != req.Weight {
				t.Fatal(score)
			}
		}
	}
}

func TestPageRankBoundsAndCancellation(t *testing.T) {
	req := pageRankRequest{IDs: []int64{1, 2}, Iterations: 6, Weight: .35}
	if !validPageRankRequest(&req) {
		t.Fatal(req)
	}
	for _, bad := range []pageRankRequest{
		{IDs: []int64{1, 1}, Iterations: 6, Weight: 1}, {IDs: []int64{-1}, Iterations: 6, Weight: 1},
		{IDs: []int64{1}, Iterations: 0, Weight: 1}, {IDs: []int64{1}, Iterations: 17, Weight: 1},
		{IDs: []int64{1}, Iterations: 1, Weight: math.NaN()}, {IDs: []int64{1}, Iterations: 1, Weight: math.Inf(1)},
		{IDs: []int64{1}, Iterations: 1, Weight: 11}, {IDs: []int64{1}, Iterations: 1, Weight: 0},
		{IDs: []int64{1}, Iterations: 1, Weight: 1, Relations: []string{""}},
	} {
		if validPageRankRequest(&bad) {
			t.Fatal("accepted", bad)
		}
	}
	if _, _, err := pageRankScores(context.Background(), make([]int64, 129), nil, req); err == nil {
		t.Fatal("unbounded candidates")
	}
	if _, _, err := pageRankScores(context.Background(), req.IDs, make([]MemoryLink, 8193), req); err == nil {
		t.Fatal("unbounded links")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, err := pageRankScores(ctx, req.IDs, nil, req); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	raw, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"pagerank","ids":[1,2]}`))
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 99}, raw); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("non-host admitted", status)
	}
	for _, body := range []string{`{"operation":"pagerank"}`, `{"operation":"pagerank","ids":[1,1]}`, `{"operation":"pagerank","ids":[1],"weight":null}`, `{"operation":"pagerank","ids":[1],"iterations":null}`, `{"operation":"pagerank","ids":[1],"relations":null}`, `{"operation":"pagerank","ids":[1],"iterations":17}`} {
		raw, _ := bus.EncodeCommand("runtime", []byte(body))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, raw); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(body, status)
		}
	}
}

func exercisePageRankReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT pagerank_replay`)
	defer exec(`ROLLBACK TO SAVEPOINT pagerank_replay; RELEASE SAVEPOINT pagerank_replay`)
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	ids := []int64{}
	for i, state := range []string{"active", "active", "active", "active", "suppressed", "archived", "private", "global"} {
		id := int64(9007199254743799 + i)
		scope, scopeValue, life, suppressed := "project", "pagerank-local", "active", 0
		if state == "suppressed" {
			suppressed = 1
		}
		if state == "archived" {
			life = "archived"
		}
		if state == "private" {
			scopeValue = "pagerank-private"
		}
		if state == "global" {
			scope, scopeValue = "global", "_global"
		}
		exec(`INSERT INTO memories(id,tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed) VALUES($1,'L2','fact','pagerank-replay','needle',$2,$3,$4,$5)`, id, scope, scopeValue, life, suppressed)
		ids = append(ids, id)
	}
	for _, id := range ids[1:] {
		exec(`INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'depends_on')`, ids[0], id)
	}
	exec(`INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'ignored')`, ids[1], ids[2])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	args := map[string]any{"operation": "pagerank", "ids": ids, "project": "pagerank-local", "iterations": 8, "weight": 1.2, "relations": []string{"depends_on"}}
	call := func() (pageRankResult, bus.ModuleStatus) {
		t.Helper()
		raw, _ := json.Marshal(args)
		frame, _ := bus.EncodeCommand("runtime", raw)
		reply, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		var result pageRankResult
		if status == bus.ModuleStatusOK {
			raw, err := bus.DecodeCommandResult(reply)
			if err != nil || json.Unmarshal(raw, &result) != nil {
				t.Fatal(string(raw), err)
			}
		}
		return result, status
	}
	before := pageRankMetricState.snapshot()
	got, status := call()
	if status != bus.ModuleStatusOK || got.Candidates != 5 || len(got.Scores) != 5 || got.Edges != 8 || got.Scores[0].ID != ids[0] || got.Scores[0].Score != 1.2 {
		t.Fatal(got, status)
	}
	if pageRankMetricState.snapshot().Samples != before.Samples+1 {
		t.Fatal("successful scorer not measured")
	}
	for _, score := range got.Scores {
		if score.ID == ids[4] || score.ID == ids[5] || score.ID == ids[6] {
			t.Fatal("ineligible candidate influenced graph", score)
		}
	}
	args["scope"] = Scope{Type: ScopeProject, Value: "pagerank-local"}
	args["include_all"] = true
	got, status = call()
	if status != bus.ModuleStatusOK || got.Candidates != 4 || got.Edges != 6 {
		t.Fatal("exact scope widened", got, status)
	}
	delete(args, "scope")
	delete(args, "project")
	delete(args, "include_all")
	got, status = call()
	if status != bus.ModuleStatusOK || got.Candidates != 1 || len(got.Scores) != 0 {
		t.Fatal("context leaked", got, status)
	}
	args["project"] = "pagerank-local"
	exec(`RESET ROLE; INSERT INTO memory_links(source_id,target_id,relation) SELECT 9007199254743799,9007199254743800,'depends_on' FROM generate_series(1,8193); SET LOCAL ROLE aimee_store_runtime`)
	before = pageRankMetricState.snapshot()
	if _, status = call(); status != bus.ModuleStatusInternal {
		t.Fatal("graph overflow accepted", status)
	}
	if !reflect.DeepEqual(before, pageRankMetricState.snapshot()) {
		t.Fatal("failed scoring recorded as success")
	}
	exec(`RESET ROLE; REVOKE SELECT ON memory_links FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	if _, status = call(); status != bus.ModuleStatusInternal {
		t.Fatal("SQL refusal treated as no links", status)
	}
}

func BenchmarkPageRankKernel50(b *testing.B) {
	ids := make([]int64, 50)
	links := make([]MemoryLink, 49)
	for i := range ids {
		ids[i] = int64(i + 1)
		if i > 0 {
			links[i-1] = MemoryLink{SourceID: 1, TargetID: ids[i], Relation: "depends_on"}
		}
	}
	req := pageRankRequest{IDs: ids, Iterations: 8, Weight: 1.2, Relations: []string{"depends_on"}}
	b.ResetTimer()
	for range b.N {
		if _, _, err := pageRankScores(context.Background(), ids, links, req); err != nil {
			b.Fatal(err)
		}
	}
}

func TestPageRankMetricsConcurrent(t *testing.T) {
	var counters pageRankMetricCounters
	if counters.snapshot() != (pageRankMetrics{}) {
		t.Fatal("new counters not empty")
	}
	var wg sync.WaitGroup
	for i := 1; i <= 32; i++ {
		wg.Add(1)
		go func(ms int) {
			defer wg.Done()
			counters.observe(pageRankResult{ElapsedMS: float64(ms), Candidates: 5, Edges: 8})
			_ = counters.snapshot()
		}(i)
	}
	wg.Wait()
	got := counters.snapshot()
	if got.Samples != 32 || math.Abs(got.AverageMS-16.5) > 1e-12 || got.MaximumMS != 32 || got.Candidates != 5 || got.Edges != 8 {
		t.Fatal(got)
	}
	for _, timing := range []pageRankMetrics{{}, got} {
		result := map[string]any{}
		addStatsConsole(result, MemoryStats{}, timing)
		view := result["pagerank_timing"].(map[string]any)
		expected := "measured"
		if timing.Samples == 0 {
			expected = "unmeasured"
		}
		if view["state"] != expected || view["source"] != "candidate-scorer" || result["display"].(map[string]any)["pagerank"] != timing {
			t.Fatal(result)
		}
	}
}
