package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"reflect"
	"strconv"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
)

func cleanPageRankEnv(t *testing.T) {
	t.Helper()
	for _, key := range []string{"ENABLED", "ITERATIONS", "WEIGHT", "RELATIONS"} {
		t.Setenv("AIMEE_MEMORY_PAGERANK_"+key, "")
	}
}

func TestPageRankRecallConfiguration(t *testing.T) {
	cleanPageRankEnv(t)
	cfg, err := loadPageRankConfig(nil)
	if err != nil || cfg.enabled || cfg.request.Iterations != 6 || cfg.request.Weight != .35 {
		t.Fatal(cfg, err)
	}
	configured := map[string]any{"memory_pagerank_enabled": true, "memory_pagerank_iterations": 8, "memory_pagerank_weight": 1.2, "memory_pagerank_relations": " depends_on, fixes "}
	cfg, err = loadPageRankConfig(configured)
	if err != nil || !cfg.enabled || cfg.request.Iterations != 8 || cfg.request.Weight != 1.2 || !reflect.DeepEqual(cfg.request.Relations, []string{"depends_on", "fixes"}) {
		t.Fatal(cfg, err)
	}
	for _, bad := range []map[string]any{
		{"memory_pagerank_enabled": math.NaN()},
		{"memory_pagerank_enabled": true, "memory_pagerank_iterations": 1.5},
		{"memory_pagerank_enabled": true, "memory_pagerank_weight": math.Inf(1)},
		{"memory_pagerank_enabled": true, "memory_pagerank_relations": " , "},
	} {
		if _, err := loadPageRankConfig(bad); err == nil {
			t.Fatal("invalid config accepted", bad)
		}
	}
	t.Setenv("AIMEE_MEMORY_PAGERANK_ENABLED", "0")
	cfg, err = loadPageRankConfig(configured)
	if err != nil || cfg.enabled {
		t.Fatal(cfg, err)
	}
	t.Setenv("AIMEE_MEMORY_PAGERANK_ENABLED", "1")
	t.Setenv("AIMEE_MEMORY_PAGERANK_ITERATIONS", "100")
	t.Setenv("AIMEE_MEMORY_PAGERANK_WEIGHT", "2")
	t.Setenv("AIMEE_MEMORY_PAGERANK_RELATIONS", "related_to")
	cfg, err = loadPageRankConfig(configured)
	if err != nil || cfg.request.Iterations != 16 || cfg.request.Weight != 2 || !reflect.DeepEqual(cfg.request.Relations, []string{"related_to"}) {
		t.Fatal(cfg, err)
	}
	t.Setenv("AIMEE_MEMORY_PAGERANK_WEIGHT", "NaN")
	if _, err := loadPageRankConfig(configured); err == nil {
		t.Fatal("non-finite env accepted")
	}
	// Server bypasses KB graph configuration, including a missing config provider.
	backend := postgresDataStore{placement: PlacementServer, settings: func() (map[string]any, error) { return nil, errors.New("offline") }}
	req, err := backend.planRecall(DataRequest{Query: "needle", Limit: 5})
	if err != nil || req.Limit != 5 || req.pageRankConfig != nil {
		t.Fatal(req, err)
	}
}

type pageRankFailCommitDB struct{ runtimeRoleDB }
type pageRankFailCommitTx struct{ store.Tx }

func (db pageRankFailCommitDB) Begin(ctx context.Context) (store.Tx, error) {
	tx, err := db.runtimeRoleDB.Begin(ctx)
	return pageRankFailCommitTx{tx}, err
}
func (tx pageRankFailCommitTx) Commit(context.Context) error {
	return errors.New("fixture commit failure")
}

func exercisePageRankRecallReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	cleanPageRankEnv(t)
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT pagerank_recall_replay`)
	defer exec(`ROLLBACK TO SAVEPOINT pagerank_recall_replay; RELEASE SAVEPOINT pagerank_recall_replay`)
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	ids := map[string]int64{}
	for i, key := range []string{"hub", "left", "right", "tail", "neighbor", "private", "suppressed", "archived", "wrong-kind", "wrong-tier", "global", "ignored", "future", "expired"} {
		scope, value, life, kind, tier, suppressed, content := "project", "pagerank-recall-local", "active", "fact", "L2", 0, "rankneedle"
		switch key {
		case "neighbor", "ignored":
			content = "unrelated payload"
		case "private":
			value = "pagerank-recall-private"
		case "suppressed":
			suppressed = 1
		case "archived":
			life = "archived"
		case "wrong-kind":
			kind = "procedure"
		case "wrong-tier":
			tier = "L1"
		case "global":
			scope, value = "global", "_global"
		}
		id := int64(9007199254744800 + i)
		exec(`INSERT INTO memories(id,tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed,updated_at) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,'2026-01-01')`, id, tier, kind, "pagerank-"+key, content, scope, value, life, suppressed)
		ids[key] = id
	}
	exec(`UPDATE memories SET valid_from=(now()+interval '1 second')::text WHERE id=$1`, ids["future"])
	exec(`UPDATE memories SET valid_until=now()::text WHERE id=$1`, ids["expired"])
	for _, key := range []string{"left", "right", "tail", "neighbor", "private", "suppressed", "archived", "wrong-kind", "wrong-tier", "global", "future", "expired"} {
		exec(`INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'depends_on')`, ids["hub"], ids[key])
	}
	exec(`INSERT INTO memory_links(source_id,target_id,relation) VALUES($1,$2,'ignored')`, ids["hub"], ids["ignored"])
	// Hidden edges exceed the work cap but cannot spend the visible graph budget.
	exec(`INSERT INTO memory_links(source_id,target_id,relation) SELECT $1,$2,'depends_on' FROM generate_series(1,8200)`, ids["hub"], ids["private"])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	bound := *backend
	bound.fusionEnabled = false
	values := map[string]any{"memory_pagerank_enabled": false}
	bound.settings = func() (map[string]any, error) { return values, nil }
	handler := NewHandler(nil, WithDataStore(PlacementKB, &bound))
	req := DataRequest{Operation: "search", Query: "rankneedle", Project: "pagerank-recall-local", Kind: "fact", Tier: "L2", Limit: 1}
	call := func(h bus.ModuleHandler) (DataResponse, bus.ModuleStatus) {
		t.Helper()
		raw, _ := json.Marshal(req)
		reply, status := h(bus.ModuleInvocation{StageID: StageData}, raw)
		var result DataResponse
		if status == bus.ModuleStatusOK && json.Unmarshal(reply, &result) != nil {
			t.Fatal(string(reply))
		}
		return result, status
	}
	before := pageRankMetricState.snapshot()
	got, status := call(handler)
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["tail"] || pageRankMetricState.snapshot() != before {
		t.Fatal("default ordering/metrics changed", got, status)
	}
	// Caller-supplied scorer parameters cannot enable or reconfigure retrieval.
	req.PageRank = &pageRankRequest{IDs: []int64{ids["hub"]}, Iterations: 16, Weight: 10}
	got, status = call(handler)
	if status != bus.ModuleStatusOK || got.Records[0].ID != ids["tail"] {
		t.Fatal("caller changed ranking", got, status)
	}
	req.PageRank = nil
	values["memory_pagerank_enabled"] = true
	got, status = call(handler)
	after := pageRankMetricState.snapshot()
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["hub"] || after.RecallSamples != before.RecallSamples+1 || after.Candidates != 6 || after.Edges != 10 {
		t.Fatal("PageRank did not reach pre-limit candidates", got, status, after)
	}
	req.Limit = 20
	got, status = call(handler)
	if status != bus.ModuleStatusOK || len(got.Records) != 6 || got.Records[len(got.Records)-1].ID != ids["global"] {
		t.Fatal("scope priority or graph neighbor lost", got, status)
	}
	found := false
	for _, r := range got.Records {
		if r.ID == ids["neighbor"] {
			found = true
			if !r.Version.validFor(r.ID) || r.Version.RecordRevision != "1" || r.Content != "unrelated payload" {
				t.Fatalf("unversioned neighbor: %+v", r)
			}
		}
		if r.ID == ids["ignored"] || r.ID == ids["private"] || r.ID == ids["suppressed"] || r.ID == ids["archived"] || r.ID == ids["wrong-kind"] || r.ID == ids["wrong-tier"] {
			t.Fatal("ineligible endpoint admitted", r)
		}
	}
	if !found {
		t.Fatal("one-hop neighbor omitted")
	}
	exec("SAVEPOINT pagerank_neighbor_version")
	exec("UPDATE memories SET content='revised unrelated payload' WHERE id=$1", ids["neighbor"])
	revised, revisedStatus := call(handler)
	if revisedStatus != bus.ModuleStatusOK {
		t.Fatal(revisedStatus)
	}
	found = false
	for _, r := range revised.Records {
		if r.ID == ids["neighbor"] {
			found = true
			if !r.Version.validFor(r.ID) || r.Version.RecordRevision != "2" || r.Content != "revised unrelated payload" {
				t.Fatalf("neighbor correction snapshot: %+v", r)
			}
		}
	}
	if !found {
		t.Fatal("corrected neighbor missing")
	}
	exec("ROLLBACK TO SAVEPOINT pagerank_neighbor_version; RELEASE SAVEPOINT pagerank_neighbor_version")
	req.Operation = "diagnose"
	got, status = call(handler)
	if status != bus.ModuleStatusOK || len(got.Diagnostics) != 6 {
		t.Fatal(got, status)
	}
	if got.RankingTrace == nil || len(got.RankingTrace.Candidates) < 6 {
		t.Fatal("graph candidate trace missing", got)
	}
	graphObserved := false
	for _, candidate := range got.RankingTrace.Candidates {
		for _, step := range candidate.Steps {
			if step.Operation == "pagerank" {
				graphObserved = true
			}
		}
	}
	if !graphObserved {
		t.Fatal("actual graph contribution missing", got.RankingTrace)
	}
	for _, d := range got.Diagnostics {
		p := d.Parts
		if p.RankingPolicy != pageRankRecallPolicy || p.PageRank <= 0 || math.Abs(p.Total-p.RetrievalBase-p.PageRank) > 1e-12 {
			t.Fatal("diagnostic not from ranking", d)
		}
		trace := diagnosticTraceRows([]Diagnostic{d}, []publicDiagnostic{{Memory: publicMemoryRecord{ID: d.Memory.ID}, Parts: p}})[0]
		if math.Abs(trace["feature_contributions"].(map[string]float64)["post_rank_residual"]) > 1e-12 {
			t.Fatal("unexplained PageRank score", trace)
		}
	}
	req.Operation = "ask"
	values["memory_abstain_enabled"] = true
	values["memory_chunk_min_confidence"] = .5
	got, status = call(handler)
	if status != bus.ModuleStatusOK || got.Answer == nil || got.Answer.Evidence.Reason == "chunk_floor" {
		t.Fatal("PageRank units contaminated answer support", got, status)
	}
	delete(values, "memory_abstain_enabled")
	delete(values, "memory_chunk_min_confidence")
	req.Operation = "search"
	req.Scope = Scope{Type: ScopeProject, Value: "pagerank-recall-local"}
	req.IncludeAll = true
	got, status = call(handler)
	if status != bus.ModuleStatusOK || len(got.Records) != 5 {
		t.Fatal("exact scope widened", got, status)
	}
	req.IncludeAll = false
	req.Scope = Scope{}
	req.Project = ""
	got, status = call(handler)
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["global"] {
		t.Fatal("request scope leaked", got, status)
	}
	req.Project = "pagerank-recall-local"
	// A completely full initial pool still lets a graph-only endpoint compete.
	exec("SAVEPOINT full_initial_pool")
	exec(`RESET ROLE; INSERT INTO memories(id,tier,kind,key,content,scope_type,scope_value,lifecycle_state,updated_at)
 SELECT 9007199254780000+i,'L2','fact','fair-distractor-'||i,'rankneedle','project','pagerank-recall-local','active','2026-01-01' FROM generate_series(1,123) i;
 SET LOCAL ROLE aimee_store_runtime`)
	req.Operation, req.Limit = "diagnose", 100
	got, status = call(handler)
	admittedNeighbor := false
	if got.RankingTrace != nil {
		for _, candidate := range got.RankingTrace.Candidates {
			if candidate.ID == strconv.FormatInt(ids["neighbor"], 10) {
				for _, step := range candidate.Steps {
					admittedNeighbor = admittedNeighbor || step.Operation == "pagerank"
				}
			}
		}
	}
	if status != bus.ModuleStatusOK || !admittedNeighbor || len(got.Diagnostics) > 100 || got.RetrievalCapabilities == nil {
		t.Fatal("full lexical pool vetoed graph or exceeded caller cap", status, got)
	}
	exec("ROLLBACK TO SAVEPOINT full_initial_pool; RELEASE SAVEPOINT full_initial_pool")
	req.Operation, req.Limit = "search", 20
	// The graph scorer may finish but the request must commit before counting it.
	failing := bound
	failing.db = pageRankFailCommitDB{runtimeRoleDB{evalQueryer{tx}, t}}
	before = pageRankMetricState.snapshot()
	if _, status := call(NewHandler(nil, WithDataStore(PlacementKB, &failing))); status != bus.ModuleStatusInternal || pageRankMetricState.snapshot() != before {
		t.Fatal("failed commit recorded", status)
	}
	// Visible graph overflow and SQL refusal must not become healthy fallback.
	exec(`RESET ROLE; INSERT INTO memory_links(source_id,target_id,relation) SELECT 9007199254744800,9007199254744801,'depends_on' FROM generate_series(1,8200); SET LOCAL ROLE aimee_store_runtime`)
	if _, status := call(handler); status != bus.ModuleStatusInternal {
		t.Fatal("overflow silently degraded", status)
	}
	exec(`RESET ROLE; REVOKE SELECT ON memory_links FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	if _, status := call(handler); status != bus.ModuleStatusInternal {
		t.Fatal("graph refusal ignored", status)
	}
	if pageRankMetricState.snapshot() != before {
		t.Fatal("failed graph calls counted")
	}
	values["memory_pagerank_enabled"] = false
	req.Limit = 1
	got, status = call(handler)
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["tail"] {
		t.Fatal("disabled PageRank still accessed graph", got, status)
	}
}
