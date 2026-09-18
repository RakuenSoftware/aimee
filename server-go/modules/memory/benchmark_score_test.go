package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestBenchmarkScoreUsesOwnerRetrieval(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		const id = int64(9007199254740993)
		s := &benchmarkDiagnosticStore{rows: []Record{{ID: id - 1}, {ID: 2}, {ID: 3}, {ID: 4}, {ID: 5}, {ID: id}}}
		handler := NewHandler(nil, WithDataStore(placement, s))
		args := `{"operation":"benchmark-score","query":"fixture","expected_ids":["9007199254740993"]}`
		got := runHostRuntime(t, handler, args)
		if got["mrr"] != 1.0/6 || got["ndcg_5"] != float64(0) || math.Abs(got["ndcg_10"].(float64)-1/math.Log2(7)) > 1e-12 || got["recall_5"] != float64(0) || got["recall_10"] != float64(1) {
			t.Fatal(got)
		}
		if got["retrieved_ids"].([]any)[5] != "9007199254740993" {
			t.Fatal(got)
		}
		got = runHostRuntime(t, handler, strings.Replace(args, `["9007199254740993"]`, `[]`, 1))
		if got["mrr"] != float64(0) || len(got["retrieved_ids"].([]any)) != 6 {
			t.Fatal(got)
		}
		s.rows = nil
		got = runHostRuntime(t, handler, args)
		if got["mrr"] != float64(0) || len(got["retrieved_ids"].([]any)) != 0 {
			t.Fatal(got)
		}
		s.rows = []Record{{ID: id}}
		many := strings.Replace(args, `["9007199254740993"]`, `[`+strings.Repeat(`"9007199254740993",`, 127)+`"9007199254740993"]`, 1)
		got = runHostRuntime(t, handler, many)
		if got["recall_5"] != 1.0/128 {
			t.Fatal("support relevance cap or duplicate semantics changed", got)
		}
		s.searchErr = errors.New("index unavailable")
		frame, _ := bus.EncodeCommand("runtime", []byte(args))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
			t.Fatal("failed retrieval reported as zero score")
		}
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("peer obtained host-only evaluation", status)
		}
		before := s.searched
		for _, bad := range []string{
			strings.Replace(args, `["9007199254740993"]`, `[9007199254740993]`, 1),
			strings.Replace(args, `["9007199254740993"]`, `null`, 1),
			strings.Replace(args, `["9007199254740993"]`, `["9223372036854775808"]`, 1),
			strings.Replace(args, `["9007199254740993"]`, `[`+strings.Repeat(`"1",`, 128)+`"1"]`, 1),
		} {
			frame, _ := bus.EncodeCommand("runtime", []byte(bad))
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(bad, status)
			}
		}
		if s.searched != before {
			t.Fatal("invalid request reached search")
		}
	}
}

func exerciseBenchmarkScoreReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT benchmark_score_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT benchmark_score_replay; RELEASE SAVEPOINT benchmark_score_replay`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	var local, global, private, archived int64
	for _, row := range []struct {
		kind, scope, state string
		id                 *int64
	}{
		{"project", "benchmark-score-local", "active", &local},
		{"global", "_global", "active", &global},
		{"project", "benchmark-score-private", "active", &private},
		{"project", "benchmark-score-local", "archived", &archived},
	} {
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value,lifecycle_state) VALUES ('L2','fact','benchmark-score-replay','benchmark-score-replay',.5,$1,$2,$3) RETURNING id`, row.kind, row.scope, row.state).Scan(row.id); err != nil {
			t.Fatal(err)
		}
	}
	args := map[string]any{"operation": "benchmark-score", "query": "benchmark-score-replay", "project": "benchmark-score-local", "expected_ids": []string{strconv.FormatInt(local, 10)}}
	run := func() map[string]any { raw, _ := json.Marshal(args); return runHostRuntime(t, handler, string(raw)) }
	result := run()
	if result["mrr"] != float64(1) || !reflect.DeepEqual(result["retrieved_ids"], []any{strconv.FormatInt(local, 10), strconv.FormatInt(global, 10)}) {
		t.Fatal(result)
	}
	args["expected_ids"] = []string{strconv.FormatInt(private, 10), strconv.FormatInt(archived, 10)}
	result = run()
	if result["mrr"] != float64(0) || result["recall_10"] != float64(0) {
		t.Fatal("hidden/archived relevance leaked", result)
	}
	args["scope"] = Scope{Type: ScopeProject, Value: "benchmark-score-local"}
	args["include_all"] = true
	result = run()
	if !reflect.DeepEqual(result["retrieved_ids"], []any{strconv.FormatInt(local, 10)}) {
		t.Fatal("explicit scope widened", result)
	}
	delete(args, "scope")
	delete(args, "include_all")
	delete(args, "project")
	result = run()
	if !reflect.DeepEqual(result["retrieved_ids"], []any{strconv.FormatInt(global, 10)}) {
		t.Fatal("absent context widened", result)
	}
	exec(`RESET ROLE; REVOKE SELECT ON memories FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	raw, _ := json.Marshal(args)
	frame, _ := bus.EncodeCommand("runtime", raw)
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
		t.Fatal("required query failure became zero score")
	}
}
