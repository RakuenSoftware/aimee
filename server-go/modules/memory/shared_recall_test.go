package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/jackc/pgx/v5"
)

type sharedRecallExecutor struct {
	vector   []byte
	identity string
	changed  bool
	fail     bool
	health   int
}

func (e *sharedRecallExecutor) Do(_ context.Context, _ uint64, r egress.HTTPRequest) (egress.HTTPResponse, error) {
	if e.fail {
		return egress.HTTPResponse{}, errors.New("test embedder unavailable")
	}
	if strings.HasSuffix(r.TargetURL, "/health") {
		e.health++
		identity := e.identity
		if e.changed && e.health%2 == 0 {
			identity += "-changed"
		}
		body, _ := json.Marshal(map[string]string{"serving_id": identity})
		return egress.HTTPResponse{Status: 200, Body: body}, nil
	}
	return egress.HTTPResponse{Status: 200, Body: e.vector}, nil
}

func TestSharedSemanticFloorScale(t *testing.T) {
	for _, row := range []struct {
		dim              int
		configured, want float64
	}{
		{384, 0, 1}, {1024, 0, .55}, {2560, 0, .75}, {4000, 0, .65}, {1024, .9, .9}, {1024, math.NaN(), .55}, {1024, math.Inf(1), .55},
	} {
		if got := sharedSemanticFloorScale(row.dim, row.configured); got != row.want {
			t.Fatal(row, got)
		}
	}
}

func exerciseSharedRecallReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	resetBreaker(t)
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT shared_recall_replay`)
	defer exec(`ROLLBACK TO SAVEPOINT shared_recall_replay; RELEASE SAVEPOINT shared_recall_replay`)
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	var dimension int
	if err := tx.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute WHERE attrelid='memory_embeddings'::regclass AND attname='embedding'`).Scan(&dimension); err != nil {
		t.Fatal(err)
	}
	vector := make([]float32, dimension)
	vector[0] = 1
	encoded, _ := json.Marshal(vector)
	exec(`INSERT INTO memory_embedder_versions(version,command,dimension,serving_id) VALUES('shared-recall-test','http://shared-recall-test',$1,'test-model')`, dimension)
	exec(`INSERT INTO memory_active_embedder(id,version) VALUES(1,'shared-recall-test') ON CONFLICT(id) DO UPDATE SET version=EXCLUDED.version`)
	ids := map[string]int64{}
	for _, seed := range []struct{ key, scope, kind string }{
		{"private", "shared-recall-private", "fact"},
		{"future", "shared-recall-local", "fact"}, {"expired", "shared-recall-local", "fact"},
		{"stale", "shared-recall-local", "fact"}, {"moved", "shared-recall-local", "fact"},
		{"suppressed", "shared-recall-local", "fact"}, {"retired", "shared-recall-local", "fact"},
		{"wrong-kind", "shared-recall-local", "procedure"}, {"wrong-dim", "shared-recall-local", "fact"},
		{"zero", "shared-recall-local", "fact"}, {"unversioned", "shared-recall-local", "fact"}, {"visible", "shared-recall-local", "fact"},
	} {
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value)
 VALUES('L2',$1,$2,'opaque content',.8,'project',$3) RETURNING id`, seed.kind, "shared-recall-"+seed.key, seed.scope).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids[seed.key] = id
	}
	exec(embeddingInputs()+`INSERT INTO memory_embedding_versions(version,point_id,memory_id,input_hash,embedding)
 SELECT 'shared-recall-test',point_id,memory_id,input_hash,$1::vector FROM inputs WHERE input_key LIKE 'shared-recall-%'`, string(encoded))
	// A perfectly matching legacy vector without the active generation is not
	// semantically admissible. Re-embedding is required, not dimension guessing.
	exec(`DELETE FROM memory_embedding_versions WHERE memory_id=$1`, ids["unversioned"])
	exec(`INSERT INTO memory_embeddings(point_id,embedding,record_type,primary_scope,project,kind,payload_json) VALUES($1,$2::vector,'memory','project','shared-recall-local','fact','{}')`, ids["unversioned"], string(encoded))
	exec(`UPDATE memories SET content='edited content' WHERE id=$1`, ids["stale"])
	exec(`UPDATE memories SET scope_value='shared-recall-moved' WHERE id=$1`, ids["moved"])
	exec(`UPDATE memories SET activation_suppressed=1 WHERE id=$1`, ids["suppressed"])
	exec(`UPDATE memories SET valid_from=(now()+interval '1 second')::text WHERE id=$1`, ids["future"])
	exec(`UPDATE memories SET valid_until=now()::text WHERE id=$1`, ids["expired"])
	exec(`UPDATE memories SET lifecycle_state='archived' WHERE id=$1`, ids["retired"])
	exec(`UPDATE memory_embedding_versions SET embedding='[1,0]'::vector WHERE point_id=$1 AND version='shared-recall-test'`, ids["wrong-dim"])
	zero, _ := json.Marshal(make([]float32, dimension))
	exec(`UPDATE memory_embedding_versions SET embedding=$2::vector WHERE point_id=$1 AND version='shared-recall-test'`, ids["zero"], string(zero))
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	model := &sharedRecallExecutor{vector: encoded, identity: "test-model"}
	handler := NewHandler(model, WithDataStore(PlacementKB, backend))
	request := DataRequest{Operation: "search", Query: "utterly unrelated request", Project: "shared-recall-local", Kind: "fact", Limit: 20}
	call := func() (DataResponse, bus.ModuleStatus) {
		t.Helper()
		raw, _ := json.Marshal(request)
		body, status := handler(bus.ModuleInvocation{StageID: StageData}, raw)
		var result DataResponse
		if len(body) > 0 && json.Unmarshal(body, &result) != nil {
			t.Fatal(string(body))
		}
		return result, status
	}
	beforeLanes := laneMetrics()
	got, status := call()
	afterLanes := laneMetrics()
	if afterLanes["memory.query.lane.semantic.served"] != beforeLanes["memory.query.lane.semantic.served"]+1 ||
		afterLanes["memory.query.lane.lexical.served"] != beforeLanes["memory.query.lane.lexical.served"] {
		t.Fatal("semantic-only result attribution lost", beforeLanes, afterLanes)
	}
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["visible"] {
		t.Fatal("semantic-only recall failed or leaked hidden/stale rows", got, status)
	}
	request.Operation = "diagnose"
	traced, tracedStatus := call()
	if tracedStatus != bus.ModuleStatusOK || traced.RankingTrace == nil || len(traced.RankingTrace.Candidates) != 1 {
		t.Fatal("dense diagnostic trace", traced, tracedStatus)
	}
	candidate := traced.RankingTrace.Candidates[0]
	native, fused := false, false
	for _, step := range candidate.Steps {
		if step.Operation == "native_cosine_similarity_scope_priority" && math.Abs(step.Score-1) < 1e-9 {
			native = true
		}
		if step.Operation == "rrf60" {
			for _, c := range step.Contributions {
				if c.Arm == "semantic_parent" && c.Rank == 1 {
					fused = true
				}
			}
		}
	}
	if !native || !fused || candidate.Version == nil || candidate.Disposition != "selected" {
		t.Fatal("dense contribution or source version unavailable", candidate)
	}
	request.Operation = "search"
	// Multiple retrieval lanes share one request policy generation. The next
	// request must still observe a changed policy rather than a process cache.
	originalSettings := backend.settings
	settingsReads, floorScale := 0, 1.0
	backend.settings = func() (map[string]any, error) {
		settingsReads++
		return map[string]any{"memory_semantic_floor_scale": floorScale}, nil
	}
	got, status = call()
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || settingsReads != 1 {
		t.Fatal("retrieval did not share one policy snapshot", got, status, settingsReads)
	}
	floorScale = 2
	got, status = call()
	backend.settings = originalSettings
	if status != bus.ModuleStatusOK || len(got.Records) != 0 || settingsReads != 2 {
		t.Fatal("next request did not observe the changed policy", got, status, settingsReads)
	}
	request.Limit = 1
	got, status = call()
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["visible"] {
		t.Fatal("invisible or stale vectors consumed the candidate cap", got, status)
	}
	request.Scope = Scope{Type: ScopeProject, Value: "shared-recall-private"}
	request.IncludeAll = true
	got, status = call()
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["private"] {
		t.Fatal("explicit scope was widened", got, status)
	}
	request.Scope = Scope{}
	request.IncludeAll = false
	exerciseUnitRecallReplay(t, ctx, tx, backend, model, dimension)
	model.vector = []byte(`[1,0]`)
	got, status = call()
	if status != bus.ModuleStatusOK || len(got.Records) != 0 {
		t.Fatal("wrong query dimension accepted", got, status)
	}
	model.vector = encoded
	// A changed provider or failed embedding cannot manufacture semantic hits.
	model.changed = true
	model.health = 0
	got, status = call()
	if status != bus.ModuleStatusOK || len(got.Records) != 0 {
		t.Fatal("mixed serving identity accepted", got, status)
	}
	model.changed = false
	model.fail = true
	request.Query = "shared-recall-visible"
	got, status = call()
	if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["visible"] {
		t.Fatal("dependency outage lost lexical recall", got, status)
	}
	// Suppression applies to lexical candidates before LIMIT, including when
	// the semantic provider is down. A high-priority suppressed match must not
	// consume the visible row's slot in either explicit or contextual scope.
	exec(`RESET ROLE`)
	exec(`UPDATE memories SET key='suppression-needle',content='suppression-needle',updated_at='9999-12-31' WHERE id=$1`, ids["suppressed"])
	exec(`UPDATE memories SET content='suppression-needle' WHERE id=$1`, ids["visible"])
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	request.Query = "suppression-needle"
	for _, scope := range []Scope{{}, {Type: ScopeProject, Value: "shared-recall-local"}} {
		request.Scope = scope
		got, status = call()
		if status != bus.ModuleStatusOK || len(got.Records) != 1 || got.Records[0].ID != ids["visible"] {
			t.Fatal("suppressed lexical match displaced visible memory", scope, got, status)
		}
	}
	request.Scope = Scope{}
	model.fail = false
	request.Query = "utterly unrelated request"
	exec(`RESET ROLE; REVOKE SELECT ON memory_embedding_versions FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	_, status = call()
	if status != bus.ModuleStatusInternal {
		t.Fatal("required vector SQL failure became healthy empty recall", status)
	}
}
