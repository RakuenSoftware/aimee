package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestBenchmarkContextBudget(t *testing.T) {
	rows := []Record{{Content: "first"}, {Content: "second"}}
	got := buildBenchmarkContext(rows, 2, 2000, 100)
	if got.Context != "[1] first\n[2] second\n" || got.Kept != 2 || got.Tokens != 6 {
		t.Fatal(got)
	}
	rows = []Record{{Content: strings.Repeat("界", 3000)}, {Content: "short"}, {Content: strings.Repeat("é", 1000)}}
	for _, budget := range []int{1, 2, 3, 4, 10, 96, 191, 192, 200, 2000} {
		for _, capacity := range []int{1, 5, 6, 7, 20, 101, 16384} {
			got = buildBenchmarkContext(rows, 3, budget, capacity)
			if len(got.Context) >= capacity || got.Tokens > budget || got.Kept > 3 || !utf8.ValidString(got.Context) {
				t.Fatal(budget, capacity, got)
			}
			tokens := 0
			for _, line := range strings.SplitAfter(got.Context, "\n") {
				if line != "" {
					tokens += (len(line) + 3) / 4
				}
			}
			if tokens != got.Tokens {
				t.Fatal("token count excludes final bytes", tokens, got)
			}
		}
	}
	full := strings.Repeat("界", 2100)
	got = buildBenchmarkContext([]Record{{Content: full}}, 1, 4000, 16384)
	if got.Context != "[1] "+full+"\n" {
		t.Fatal("legacy content truncation survived", len(got.Context))
	}
}

func TestBenchmarkContextHostBoundary(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"benchmark-context","query":"test"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 9}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
			t.Fatal("missing store became empty context")
		}
	}
}

func exerciseBenchmarkContextReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT benchmark_context_replay`)
	defer func() {
		exec(`ROLLBACK TO SAVEPOINT benchmark_context_replay; RELEASE SAVEPOINT benchmark_context_replay`)
	}()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	full := strings.Repeat("界", 2100)
	exec(`INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value) VALUES
 ('L2','fact','benchmark-context-replay',$1,.1,'project','benchmark-context-local'),
 ('L2','fact','benchmark-context-replay','private',1,'project','benchmark-context-private'),
 ('L2','fact','benchmark-context-replay','global',1,'global','_global')`, full)
	args := map[string]any{"operation": "benchmark-context", "query": "benchmark-context-replay", "project": "benchmark-context-local", "top_k": 1, "token_budget": 4000, "capacity": 16384}
	raw, _ := json.Marshal(args)
	result := runHostRuntime(t, handler, string(raw))
	if result["context"] != "[1] "+full+"\n" || result["kept"] != float64(1) {
		t.Fatal(result)
	}
	delete(args, "project")
	args["top_k"] = 10
	raw, _ = json.Marshal(args)
	result = runHostRuntime(t, handler, string(raw))
	if result["context"] != "[1] global\n" {
		t.Fatal("missing context widened visibility", result)
	}
	args["scope"] = Scope{Type: ScopeProject, Value: "benchmark-context-local"}
	args["include_all"] = true
	raw, _ = json.Marshal(args)
	result = runHostRuntime(t, handler, string(raw))
	if result["context"] != "[1] "+full+"\n" {
		t.Fatal("explicit scope widened", result)
	}
	exec(`RESET ROLE; REVOKE SELECT ON memories FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	frame, _ := bus.EncodeCommand("runtime", raw)
	if encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status == bus.ModuleStatusOK {
		t.Fatal("retrieval failure masked", string(encoded))
	}
}
