package memory

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestMaintenancePublicPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL command regression")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE SCHEMA maintenance_test;
 CREATE FUNCTION maintenance_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
 CREATE FUNCTION maintenance_test.pg_now_text(shift text) RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
 SET LOCAL search_path TO pg_temp,maintenance_test,public;
 CREATE TEMP TABLE kb_meta(key text PRIMARY KEY,value text);
 CREATE TEMP TABLE memories(id bigint PRIMARY KEY,tier text,kind text,key text,provenance_category text DEFAULT 'agent_message',epistemic_kind text DEFAULT 'world_fact',lifecycle_state text DEFAULT 'active',artifact_ref text DEFAULT '',artifact_hash text DEFAULT '',confidence double precision DEFAULT 0.8,use_count bigint DEFAULT 0,
 updated_at text DEFAULT pg_now_text(),activation_suppressed bigint DEFAULT 0,valid_until text DEFAULT '');
 CREATE TEMP TABLE memory_provenance(memory_id bigint);
 CREATE TEMP TABLE memory_links(source_id bigint,target_id bigint);
 CREATE TEMP TABLE memory_scopes(memory_id bigint);
 CREATE TEMP TABLE memory_entities(memory_id bigint);
 CREATE TEMP TABLE memory_relations(memory_id bigint);
 INSERT INTO memories(id,tier,kind,key,artifact_ref) VALUES(1,'L0','fact','',''),(2,'L2','preference','artifact','file');`)
	if err != nil {
		t.Fatal(err)
	}
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB, settings: func() (map[string]any, error) {
		return map[string]any{"memory_maintenance_enabled": true, "memory_maintenance_interval_seconds": float64(600), "memory_maintenance_summarize_enabled": true}, nil
	}}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	client := clientForHandler(t, handler)
	before := runHostRuntime(t, handler, `{"operation":"maintenance-dashboard"}`)
	if before["last"] != nil || before["config"].(map[string]any)["interval_seconds"] != float64(600) {
		t.Fatal(before)
	}
	beforeMetrics := before["metrics"].(map[string]any)
	lint := runPublicCommand(t, client, "lint", `{}`)
	if lint["issue_count"] != float64(3) || len(lint["issues"].([]any)) != 3 {
		t.Fatal(lint)
	}
	for _, issue := range lint["issues"].([]any) {
		if _, ok := issue.(map[string]any)["key"]; !ok {
			t.Fatal("empty key omitted from public lint envelope")
		}
	}
	dryReply := runPublicCommand(t, client, "maintenance_run", `{"dry_run":true,"view":"console"}`)
	dry := dryReply["summary"].(map[string]any)
	if !strings.HasSuffix(dryReply["text"].(string), " (dry-run)\n") || dryReply["display"].(map[string]any)["vector_maintenance_skipped_here"] != false {
		t.Fatal(dryReply)
	}
	if dry["dry_run"] != true || dry["memory_count_before"] != float64(2) || dry["memory_count_after"] != float64(2) {
		t.Fatal(dry)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT COUNT(*) FROM kb_meta`).Scan(&count); err != nil || count != 0 {
		t.Fatal("dry run changed the maintenance schedule", count, err)
	}
	// A replay cycle with no eligible changes still persists its report. A second
	// ordinary call is throttled; force and dry-run retain distinct meanings.
	first := runPublicCommand(t, client, "maintenance_run", `{"view":"console","modes_csv":"replay"}`)["summary"].(map[string]any)
	if first["skipped"] != false || first["dry_run"] != false {
		t.Fatal(first)
	}
	if err := tx.QueryRow(ctx, `SELECT COUNT(*) FROM kb_meta`).Scan(&count); err != nil || count != 2 {
		t.Fatal(count, err)
	}
	secondReply := runPublicCommand(t, client, "maintenance_run", `{"view":"console","modes_csv":"replay"}`)
	second := secondReply["summary"].(map[string]any)
	if secondReply["text"] != "Maintenance cycle skipped (idle guard).\n" || secondReply["display"].(map[string]any)["vector_maintenance_skipped_here"] != false {
		t.Fatal(secondReply)
	}
	if second["skipped"] != true {
		t.Fatal("recent cycle not throttled", second)
	}
	forced := runPublicCommand(t, client, "maintenance_run", `{"modes":1,"force":true,"dry_run":true}`)["summary"].(map[string]any)
	if forced["skipped"] != false || forced["dry_run"] != true {
		t.Fatal(forced)
	}
	dashboard := runHostRuntime(t, handler, `{"operation":"maintenance-dashboard"}`)
	last := dashboard["last"].(map[string]any)
	if last["modes_run"] != float64(1) || last["dry_run"] != false || last["skipped"] != false {
		t.Fatal(dashboard)
	}
	metrics := dashboard["metrics"].(map[string]any)
	if metrics["runs_total"].(float64)-beforeMetrics["runs_total"].(float64) != 3 || metrics["skips_total"].(float64)-beforeMetrics["skips_total"].(float64) != 1 || metrics["ms_max"].(float64) < 0 {
		t.Fatal(metrics)
	}
	// A model's prune-only request must stop before the default-mode sentinel
	// can reach SQL. The actual L0 row survives even with force/admin-like args.
	noop := runPublicCommand(t, client, "maintenance_run", `{"model_policy":true,"view":"model","modes":4,"force":true,"authority":40}`)
	if noop["nothing_run"] != true || noop["summary"] != nil {
		t.Fatal(noop)
	}
	if err := tx.QueryRow(ctx, `SELECT COUNT(*) FROM memories WHERE id=1 AND tier='L0'`).Scan(&count); err != nil || count != 1 {
		t.Fatal("model prune changed L0 memory", count, err)
	}
	modelDefault := runPublicCommand(t, client, "maintenance_run", `{"model_policy":true,"view":"model","force":true,"dry_run":true}`)
	if modelDefault["summary"].(map[string]any)["modes_run"] != float64(MaintenanceReplay|MaintenanceCompact) || !strings.Contains(modelDefault["text"].(string), "prune was NOT run") {
		t.Fatal(modelDefault)
	}
	_, err = tx.Exec(ctx, `TRUNCATE memories`)
	if err != nil {
		t.Fatal(err)
	}
	lint = runPublicCommand(t, client, "lint", `{}`)
	if lint["issue_count"] != float64(0) || len(lint["issues"].([]any)) != 0 {
		t.Fatal(lint)
	}
}

// Observe the inputs that reach the owner, including the default-mode sentinel.
type consoleMaintenanceStore struct {
	calls int
	recordingDataStore
	maintenanceDataStore
	modes      uint32
	force, dry bool
	skipped    bool
	err        error
}

func (s *consoleMaintenanceStore) RunMaintenance(_ context.Context, modes uint32, force, dry bool) (MaintenanceSummary, error) {
	s.calls++
	s.modes, s.force, s.dry = modes, force, dry
	return MaintenanceSummary{ModesRun: modes, DryRun: dry, Skipped: s.skipped, Promoted: 2, Demoted: 3, Expired: 4, LifecycleArchived: 5, Merged: 6, Rescored: 7, ElapsedMS: 1.25}, s.err
}

func TestMaintenanceConsoleOwner(t *testing.T) {
	s := &consoleMaintenanceStore{}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	for _, test := range []struct {
		args                string
		modes               uint32
		force, dry, handoff bool
	}{
		{`{"view":"console"}`, 0, false, false, true},
		{`{"view":"console","modes_csv":" replay,compact prune, summarize, drift, replay, unknown "}`, 31, false, false, false},
		{`{"view":"console","modes_csv":"REPLAY,replay\tcompact"}`, 0, false, false, true},
		{`{"view":"console","modes_csv":"replay","force":true,"dry_run":true}`, 1, true, true, false},
		{`{"view":"console","modes":2,"modes_csv":"replay"}`, 2, false, false, false},
	} {
		r := runPublicCommand(t, client, "maintenance_run", test.args)
		if s.modes != test.modes || s.force != test.force || s.dry != test.dry {
			t.Fatalf("%s: modes=%d force=%v dry=%v", test.args, s.modes, s.force, s.dry)
		}
		display := r["display"].(map[string]any)
		if display["status"] != "ok" || display["promoted"] != float64(2) || display["vector_maintenance_owner"] != "knowledge-service" || display["vector_maintenance_skipped_here"] != test.handoff {
			t.Fatal(r)
		}
		text := r["text"].(string)
		if !strings.HasPrefix(text, "Maintenance: promoted=2 demoted=3 expired=4 archived=5 merged=6 rescored=7 elapsed_ms=1.25") ||
			strings.Contains(text, " (dry-run)") != test.dry || strings.Contains(text, "Vector maintenance skipped here;") != test.handoff {
			t.Fatal(r)
		}
	}
	for _, state := range []struct {
		skipped, dry bool
		outcome      string
	}{
		{false, false, "The other requested modes ran."},
		{true, false, "The remaining modes were skipped by the idle guard."},
		{false, true, "The other requested modes were evaluated as a dry run."},
	} {
		s.skipped = state.skipped
		args, _ := json.Marshal(map[string]any{"view": "model", "modes": 1, "dry_run": state.dry, "prune_removed": true})
		r := runPublicCommand(t, client, "maintenance_run", string(args))
		text := r["text"].(string)
		if !strings.Contains(text, "prune was NOT run") || !strings.Contains(text, state.outcome) || !strings.HasPrefix(text, `{"modes_run":1,`) {
			t.Fatal(r)
		}
	}
	for _, args := range []string{`{"view":"model","modes":4,"prune_removed":true}`, `{"view":"model","prune_removed":true}`} {
		r := runPublicCommand(t, client, "maintenance_run", args)
		if strings.Contains(r["text"].(string), "prune was NOT run") {
			t.Fatal("caller forged a prune-removal receipt", r)
		}
	}
	s.err = errors.New("maintenance transaction failed")
	for _, view := range []string{"console", "model"} {
		body, err := client.Command(context.Background(), 73, "maintenance_run", json.RawMessage(`{"view":"`+view+`","prune_removed":true}`))
		if err == nil || len(body) != 0 {
			t.Fatalf("failed maintenance looked successful: %s, %v", body, err)
		}
	}
}

func TestModelMaintenancePlan(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		for _, tc := range []struct {
			modes   string
			run     uint32
			dropped bool
		}{
			{`null`, 3, true}, {`4`, 3, true}, {`""`, 3, true},
			{`"prune"`, 0, true}, {`"prune,summarize"`, 8, true},
			{`"replay"`, 1, false}, {`"compact summarize"`, 10, false},
			{`"drift"`, 3, true}, {`"drift,prune"`, 0, true},
			{`"REPLAY,replay\tcompact"`, 3, true},
			{`" replay, compact, prune, summarize, replay,unknown "`, 11, true},
		} {
			args := `{"operation":"maintenance-model-plan","modes":` + tc.modes + `,"force":true,"dry_run":true,"actor":"operator","authority":40,"capabilities":4294967295,"model_policy":false,"prune_removed":false}`
			r := runHostRuntime(t, handler, args)
			if r["status"] != "ok" || r["execute"] != (tc.run != 0) {
				t.Fatal(placement, tc, r)
			}
			if tc.run == 0 {
				if r["required_capability"] != "admin" || r["text"] != modelMaintenanceNoop || r["request"] != nil {
					t.Fatal(r)
				}
				continue
			}
			q := r["request"].(map[string]any)
			if r["required_capability"] != "write" || q["modes"] != float64(tc.run) || tc.run&MaintenancePrune != 0 ||
				q["force"] != true || q["dry_run"] != true || q["view"] != "model" || q["model_policy"] != true || q["prune_removed"] != tc.dropped || len(q) != 6 {
				t.Fatal(r)
			}
		}
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"maintenance-model-plan"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("peer reached host policy planner", placement, status)
		}
	}
}

func TestModelMaintenanceExecutionNeverPrunes(t *testing.T) {
	s := &consoleMaintenanceStore{}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, s)))
	for _, tc := range []struct{ modes, want uint32 }{{0, 3}, {4, 0}, {5, 1}, {7, 3}, {8, 8}, {31, 27}} {
		before := s.calls
		args, _ := json.Marshal(map[string]any{"modes": tc.modes, "model_policy": true, "view": "model", "force": true, "dry_run": false})
		r := runPublicCommand(t, client, "maintenance_run", string(args))
		if tc.want == 0 {
			if s.calls != before || r["nothing_run"] != true || r["text"] != modelMaintenanceNoop {
				t.Fatal("prune-only request reached maintenance store", r)
			}
		} else if s.calls != before+1 || s.modes != tc.want || s.modes&MaintenancePrune != 0 {
			t.Fatal(tc, s.modes, r)
		}
	}
	// The operator and scheduler still own the unrestricted path.
	runPublicCommand(t, client, "maintenance_run", `{"modes":4,"force":true}`)
	if s.modes != MaintenancePrune {
		t.Fatal(s.modes)
	}
}
