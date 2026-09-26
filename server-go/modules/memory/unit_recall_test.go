package memory

import (
	"context"
	"encoding/json"
	"math"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestUnitSemanticPolicyNativeWeights(t *testing.T) {
	// Values from memory_unit_semantic_type_boost and
	// memory_unit_kind_intent_boost at 3d48beb23b^, including the caller's .03
	// unit weight and temporal admission-floor adjustments.
	for _, row := range []struct {
		query, typ, kind string
		score, floor     float64
	}{
		{"When did this happen?", "temporal", "episodic", .49, .46},
		{"last week", "event", "episodic", .41, .46},
		{"what day", "summary", "episodic", .28, .58},
		{"who owns this", "entity", "semantic", .43, .52},
		{"team", "event", "episodic", .18, .52},
		{"how to deploy", "chunk", "procedural", .43, .52},
		{"setup", "summary", "semantic", .13, .52},
		{"update candidate ranker in Chicago", "temporal", "episodic", .10, .52},
		{"birthday", "event", "semantic", .30, .52},
	} {
		p := semanticUnitPolicy(answerIntent(row.query))
		kind, ok := p.Kinds[row.kind]
		if !ok {
			kind = p.OtherKind
		}
		score := .1 + .03 + p.Types[row.typ] + kind
		floor, ok := p.Floors[row.typ]
		if !ok {
			floor = .52
		}
		if math.Abs(score-row.score) > 1e-12 || floor != row.floor {
			t.Fatal(row, score, floor)
		}
	}
}

func exerciseUnitRecallReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore, model *sharedRecallExecutor, dimension int) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT unit_recall_replay`)
	defer exec(`ROLLBACK TO SAVEPOINT unit_recall_replay; RELEASE SAVEPOINT unit_recall_replay`)
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	weak := make([]float32, dimension)
	// The default admission floor is higher for <=384 dimensions. Keep
	// intended unit lanes above that floor, while summary and unrelated
	// intents remain below it; the production dimension policy is unchanged.
	similarity := .1
	if dimension <= 384 {
		similarity = .25
	}
	weak[0], weak[1] = float32(similarity), float32(math.Sqrt(1-similarity*similarity))
	strong := make([]float32, dimension)
	strong[0] = 1
	off := make([]float32, dimension)
	off[1] = 1
	weakJSON, _ := json.Marshal(weak)
	strongJSON, _ := json.Marshal(strong)
	offJSON, _ := json.Marshal(off)
	ids := map[string]int64{}
	units := map[string]int64{}
	for _, key := range []string{"temporal", "event", "summary", "entity", "procedural", "both", "private", "suppressed", "archived", "stale-parent", "stale-unit", "wrong-kind", "wrong-tier", "wrong-dim", "zero", "nan", "future", "expired"} {
		scope, kind, tier, typ, unitKind := "unit-recall-local", "fact", "L2", "temporal", "episodic"
		switch key {
		case "private":
			scope = "unit-recall-private"
		case "wrong-kind":
			kind = "procedure"
		case "wrong-tier":
			tier = "L1"
		case "event", "summary":
			typ = key
		case "entity":
			typ, unitKind = "entity", "semantic"
		case "procedural":
			typ, unitKind = "chunk", "procedural"
		}
		var id, unitID int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES($1,$2,$3,'opaque source','project',$4) RETURNING id`, tier, kind, "unit-recall-"+key, scope).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids[key] = id
		if err := tx.QueryRow(ctx, `INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text,memory_kind,weight) VALUES($1,$2,'opaque','opaque derived text',$3,1) RETURNING id`, id, typ, unitKind).Scan(&unitID); err != nil {
			t.Fatal(err)
		}
		units[key] = unitID
	}
	// Several qualifying units from one parent must not crowd out another parent.
	exec(`INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text,weight,memory_kind) SELECT $1,'temporal',n::text,'opaque derived text',1,'episodic' FROM generate_series(1,4) n`, ids["temporal"])
	exec(`UPDATE memory_units SET weight='NaN'::double precision WHERE id=$1`, units["nan"])
	exec(embeddingInputs()+`INSERT INTO memory_embedding_versions(version,point_id,memory_id,input_hash,embedding)
 SELECT 'shared-recall-test',point_id,memory_id,input_hash,CASE WHEN record_type='unit' THEN $1::vector ELSE $2::vector END
 FROM inputs WHERE scope_value LIKE 'unit-recall-%'`, string(weakJSON), string(offJSON))
	exec(`UPDATE memory_embedding_versions SET embedding=$2::vector WHERE version='shared-recall-test' AND point_id=$1`, ids["both"], string(strongJSON))
	exec(`UPDATE memories SET activation_suppressed=1 WHERE id=$1`, ids["suppressed"])
	exec(`UPDATE memories SET valid_from=(now()+interval '1 second')::text WHERE id=$1`, ids["future"])
	exec(`UPDATE memories SET valid_until=now()::text WHERE id=$1`, ids["expired"])
	exec(`UPDATE memories SET lifecycle_state='archived' WHERE id=$1`, ids["archived"])
	exec(`UPDATE memories SET content='changed source' WHERE id=$1`, ids["stale-parent"])
	exec(`UPDATE memory_units SET unit_text='changed derived text' WHERE id=$1`, units["stale-unit"])
	exec(`UPDATE memory_embedding_versions SET embedding='[1,0]'::vector WHERE version='shared-recall-test' AND point_id=1000000000000+$1`, units["wrong-dim"])
	zero, _ := json.Marshal(make([]float32, dimension))
	exec(`UPDATE memory_embedding_versions SET embedding=$2::vector WHERE version='shared-recall-test' AND point_id=1000000000000+$1`, units["zero"], string(zero))
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	handler := NewHandler(model, WithDataStore(PlacementKB, backend))
	request := DataRequest{Operation: "search", Query: "when did this happen", Project: "unit-recall-local", Kind: "fact", Tier: "L2", Limit: 20}
	call := func(want ...string) {
		t.Helper()
		raw, _ := json.Marshal(request)
		reply, status := handler(bus.ModuleInvocation{StageID: StageData}, raw)
		var got DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(reply, &got) != nil || len(got.Records) != len(want) {
			t.Fatal(request, string(reply), status, want)
		}
		for i, key := range want {
			if got.Records[i].ID != ids[key] {
				t.Fatal("unit ranking/deduplication/eligibility", request, got.Records, want)
			}
		}
	}
	before := laneMetrics()
	call("both", "temporal", "event")
	after := laneMetrics()
	if after["memory.query.lane.unit.served"] != before["memory.query.lane.unit.served"]+3 || after["memory.query.lane.temporal.served"] != before["memory.query.lane.temporal.served"]+2 {
		t.Fatal("unit attribution lost", before, after)
	}
	request.Limit = 2
	call("both", "temporal")
	// Unit-only records remain retrievable without a matching whole-row vector.
	request.Query = "who is responsible"
	call("both", "entity")
	request.Query = "how to configure"
	call("both", "procedural")
	request.Query = "update candidate ranker in Chicago"
	call("both")
	request.Query = "when did this happen"
	request.Scope = Scope{Type: ScopeProject, Value: "unit-recall-private"}
	request.IncludeAll = true
	call("private")
	request.Scope = Scope{}
	request.IncludeAll = false
	request.Project = ""
	request.Query = "when did this happen"
	call()
	// A SQL refusal in the unit channel is an error, not successful empty recall.
	request.Project = "unit-recall-local"
	exec(`RESET ROLE; REVOKE SELECT ON memory_units FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	raw, _ := json.Marshal(request)
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, raw); status != bus.ModuleStatusInternal {
		t.Fatal("unit query refusal hidden", status)
	}
}
