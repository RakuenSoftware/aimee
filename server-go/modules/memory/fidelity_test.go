package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

func TestFidelityRuntimeAdmission(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		h := NewHandler(nil, WithDataStore(placement, nil))
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"fidelity-read","turn_id":"turn"}`))
		if _, status := h(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 7}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("public principal admitted", status)
		}
		for _, turn := range []string{"", strings.Repeat("x", 129)} {
			body, _ := json.Marshal(map[string]any{"operation": "fidelity-read", "turn_id": turn})
			frame, _ := bus.EncodeCommand("runtime", body)
			if _, status := h(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal("invalid turn admitted", status)
			}
		}
		if _, status := h(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal("missing store hidden", status)
		}
	}
}

// Both roles exercise the real PostgreSQL module's encoded store contract, in a
// separate disposable database. Native-era rows need no schema rewrite.
func TestFidelityPostgresModule(t *testing.T) {
	url := os.Getenv("AIMEE_DB_TEST_URL")
	if url == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL admin DSN")
	}
	t.Setenv("AIMEE_DB2_EVAL_URL", url)
	ctx := context.Background()
	db, err := postgres.OpenEvaluationStore(ctx, `CREATE TABLE artifacts(id text PRIMARY KEY,kind text,turn_id text,created_at text,payload jsonb)`)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := db.Close(); err != nil {
			t.Error(err)
		}
	}()
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		t.Run(string(placement), func(t *testing.T) {
			if _, err := db.Exec(ctx, `DELETE FROM artifacts`); err != nil {
				t.Fatal(err)
			}
			backend, err := NewPostgresDataStore(db, placement)
			if err != nil {
				t.Fatal(err)
			}
			h := NewHandler(nil, WithDataStore(placement, backend))
			read := func(turn string) map[string]any {
				t.Helper()
				b, _ := json.Marshal(map[string]any{"operation": "fidelity-read", "turn_id": turn})
				return runHostRuntime(t, h, string(b))
			}
			if got := read("missing"); got["fidelity_status"] != "not_evaluated" || got["report"] != nil {
				t.Fatal(got)
			}
			// Match the legacy writer's payload and retain exact turn identity.
			if _, err := db.Exec(ctx, `INSERT INTO artifacts VALUES
   ('report','fidelity_report',' turn ','2026-01-01','{"status":"ok","supported":3,"unsupported":1,"abstained":2}'),
   ('chunk','fidelity_attribution',' turn ','2026-01-01','{"surfaced_id":9007199254740993,"verdict":"accepted"}'),
   ('unrelated','retrieval_attribution',' turn ','2026-01-01','{}'),
   ('other','fidelity_attribution','another-turn','2026-01-01','{}')`); err != nil {
				t.Fatal(err)
			}
			got := read(" turn ")
			report, ok := got["report"].(map[string]any)
			if !ok || got["fidelity_status"] != "ok" || got["attribution_count"] != float64(1) || report["supported"] != float64(3) || report["unsupported"] != float64(1) || report["abstained"] != float64(2) {
				t.Fatal(got)
			}
			if got := read("turn"); got["fidelity_status"] != "not_evaluated" {
				t.Fatal("turn normalized", got)
			}
			for _, payload := range []string{`{"status":"not_evaluated","supported":0,"unsupported":0,"abstained":0}`, `{"status":"evidence_unavailable"}`, `{"status":"not_instrumented"}`} {
				if _, err := db.Exec(ctx, `UPDATE artifacts SET payload=$1::jsonb WHERE id='report'`, payload); err != nil {
					t.Fatal(err)
				}
				var want map[string]any
				_ = json.Unmarshal([]byte(payload), &want)
				if got := read(" turn "); got["fidelity_status"] != want["status"] {
					t.Fatal(got)
				}
			}
			for _, payload := range []string{`null`, `[]`, `"broken"`, `{"supported":"broken"}`} {
				if _, err := db.Exec(ctx, `UPDATE artifacts SET payload=$1::jsonb WHERE id='report'`, payload); err != nil {
					t.Fatal(err)
				}
				frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"fidelity-read","turn_id":" turn "}`))
				if _, status := h(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInternal {
					t.Fatal("malformed report fabricated", payload, status)
				}
			}
		})
	}
	if _, err := db.Exec(ctx, `ALTER TABLE artifacts RENAME TO unavailable_artifacts`); err != nil {
		t.Fatal(err)
	}
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		backend, _ := NewPostgresDataStore(db, placement)
		h := NewHandler(nil, WithDataStore(placement, backend))
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"fidelity-read","turn_id":"turn"}`))
		if _, status := h(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInternal {
			t.Fatal("query failure became absent evidence", status)
		}
	}
}
