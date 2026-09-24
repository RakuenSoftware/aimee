package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestAlertViews(t *testing.T) {
	content := strings.Repeat("界", 3000) + " end-marker"
	b := alertsBundle{Stale: []staleAlert{{MemoryID: math.MaxInt64, Text: content, AgeDays: 9, WindowDays: 10}}, Conflicts: []conflictAlert{{Topic: "choice", A: "yes", B: "no"}}, Superseded: []supersededAlert{{Record: Record{Key: "old"}, MemoryID: math.MaxInt64, Text: "was true", SupersededAt: "yesterday"}}, ElapsedMS: 1.25}
	want := "# Memory Alerts\n\n## Stale Pending (1)\n  - #9223372036854775807 age=9.0d/10.0d: " + content + "\n\n## Unresolved Contradictions (1)\n  - choice\n    A: yes\n    B: no\n\n## Newly Superseded (1)\n  - #9223372036854775807 [old] was true (at yesterday)\n\nassembled in 1.25ms\n"
	raw, _ := json.Marshal(b)
	text, err := memoryBundleOutput("alerts", raw, commandArgs{"format": json.RawMessage(`"text"`)}, false)
	if err != nil || text != want {
		t.Fatal("text view", err)
	}
	text, err = memoryBundleOutput("alerts", raw, commandArgs{"format": json.RawMessage(`"json"`), "fields": json.RawMessage(`"stale_pending"`), "profile": json.RawMessage(`"compact"`)}, false)
	if err != nil || !strings.Contains(text, "9223372036854775807") || strings.Contains(text, "created_at") || strings.Contains(text, "newly_superseded") {
		t.Fatal(text, err)
	}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{"since":"not a date"}`, `{"format":"invalid"}`} {
		if r := runPublicCommand(t, client, "alerts", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
	for _, format := range []string{"", "text", "json", "mcp"} {
		if r := runPublicCommand(t, client, "alerts", `{"format":"`+format+`"}`); r["kind"] != "unavailable" || r["output"] != nil || r["alerts"] != nil {
			t.Fatal(r)
		}
	}
}

func exerciseAlertsReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT alerts_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT alerts_replay; RELEASE SAVEPOINT alerts_replay`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`UPDATE memories SET lifecycle_state='active',ttl_at=''; UPDATE memory_conflicts SET resolved=1`)
	seed := func(scope, value, key, content, state, created, ttl, updated string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,created_at,ttl_at,updated_at)
 VALUES('L2','fact',$1,$2,$3,$4,$5,pg_now_text($6),CASE WHEN $7='' THEN '' ELSE pg_now_text($7) END,pg_now_text($8)) RETURNING id`, key, content, scope, value, state, created, ttl, updated).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	local := func(key, content, state, created, ttl, updated string) int64 {
		return seed("project", "alerts-project", key, content, state, created, ttl, updated)
	}
	full := strings.Repeat("界", 1000) + " alerts-tail"
	stale := local("long-window", full, "pending", "-90 days", "+10 days", "0 days")
	boundary := local("boundary", "80 percent", "pending", "-8 days", "+2 days", "0 days")
	expired := local("expired", "past due", "pending", "-10 days", "-1 days", "0 days")
	fresh := local("short-window", "not stale", "pending", "-1 hour", "+1 day", "0 days")
	before := local("before", "70 percent", "pending", "-7 days", "+3 days", "0 days")
	invalidWindow := local("invalid-window", "reversed", "pending", "0 days", "-1 days", "0 days")
	noTTL := local("no-ttl", "none", "pending", "-90 days", "", "0 days")
	workspace := seed("workspace", "alerts-workspace", "workspace", "workspace stale", "pending", "-9 days", "+1 day", "0 days")
	hidden := seed("project", "alerts-private", "private", "hidden stale", "pending", "-90 days", "+1 day", "0 days")
	recent := local("recent", full, "superseded", "-4 days", "", "-3 days")
	old := local("old", "old value", "superseded", "-20 days", "", "-8 days")
	wsRecent := seed("workspace", "alerts-workspace", "workspace-recent", "workspace old", "superseded", "-1 day", "", "0 days")
	a := local("choice", full, "active", "-1 day", "", "0 days")
	bb := local("choice", "other value", "active", "-1 day", "", "0 days")
	conflict := func(a, b int64, when string, resolved int) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memory_conflicts(memory_a,memory_b,detected_at,resolved) VALUES($1,$2,pg_now_text($3),$4) RETURNING id`, a, b, when, resolved).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	visibleConflict := conflict(a, bb, "-3 days", 0)
	hiddenConflict := conflict(a, hidden, "0 days", 0)
	resolvedConflict := conflict(bb, workspace, "0 days", 1)
	run := func(extra map[string]any) (alertsBundle, map[string]json.RawMessage) {
		t.Helper()
		args := map[string]any{"scope_context": true, "project": "alerts-project", "workspace": "alerts-workspace"}
		for k, v := range extra {
			args[k] = v
		}
		raw, _ := json.Marshal(args)
		frame, _ := bus.EncodeCommand("alerts", raw)
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		body, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var envelope struct {
			Status string          `json:"status"`
			Alerts json.RawMessage `json:"alerts"`
			Output string          `json:"output"`
		}
		if json.Unmarshal(body, &envelope) != nil || envelope.Status != "ok" {
			t.Fatal(string(body))
		}
		payload := envelope.Alerts
		if envelope.Output != "" {
			payload = []byte(envelope.Output)
		}
		var b alertsBundle
		var fields map[string]json.RawMessage
		if json.Unmarshal(payload, &b) != nil || json.Unmarshal(payload, &fields) != nil {
			t.Fatal(string(payload))
		}
		if b.ElapsedMS < 0 {
			t.Fatal("negative elapsed", b.ElapsedMS)
		}
		return b, fields
	}
	b, _ := run(nil)
	if len(b.Stale) != 4 || b.Stale[0].MemoryID != stale || b.Stale[0].Text != full || math.Abs(b.Stale[0].WindowDays-100) > .001 || b.Stale[0].AgeDays < 90 {
		t.Fatal("stale policy", b.Stale)
	}
	for _, r := range b.Stale {
		if r.MemoryID == fresh || r.MemoryID == before || r.MemoryID == invalidWindow || r.MemoryID == noTTL || r.MemoryID == hidden {
			t.Fatal("excluded pending", r)
		}
	}
	if b.Stale[1].MemoryID != expired || b.Stale[2].MemoryID != boundary || b.Stale[3].MemoryID != workspace {
		t.Fatal("scope and age order", b.Stale)
	}
	if len(b.Conflicts) != 1 || b.Conflicts[0].ConflictID != visibleConflict || b.Conflicts[0].A != full || b.Conflicts[0].B != "other value" || b.Conflicts[0].MemoryIDs[1] != bb {
		t.Fatal(b.Conflicts)
	}
	if len(b.Superseded) != 2 || b.Superseded[0].MemoryID != recent || b.Superseded[0].Text != full || b.Superseded[1].MemoryID != wsRecent {
		t.Fatal(b.Superseded)
	}
	b, fields := run(map[string]any{"format": "json", "profile": "compact", "fields": "stale_pending,newly_superseded"})
	if len(fields) != 2 || len(b.Stale) != 4 || b.Stale[0].CreatedAt != "" {
		t.Fatal(fields)
	}
	b, fields = run(map[string]any{"format": "mcp", "project": "", "workspace": ""})
	if string(fields["active_context_missing"]) != "true" || len(b.Stale) != 0 || len(b.Conflicts) != 0 || len(b.Superseded) != 0 {
		t.Fatal(b, fields)
	}
	b, _ = run(map[string]any{"include_all": true})
	foundPrivate := false
	for _, r := range b.Stale {
		foundPrivate = foundPrivate || r.MemoryID == hidden
	}
	if !foundPrivate || len(b.Conflicts) != 2 {
		t.Fatal("explicit all", b)
	}
	var cutoff string
	if err := tx.QueryRow(ctx, `SELECT pg_now_text('-9 days')`).Scan(&cutoff); err != nil {
		t.Fatal(err)
	}
	when, err := parseMemoryTime(cutoff)
	if err != nil {
		t.Fatal(err)
	}
	for _, since := range []string{cutoff, when.In(time.FixedZone("offset", 7200)).Format(time.RFC3339)} {
		b, _ = run(map[string]any{"since": since})
		if len(b.Superseded) != 3 || b.Superseded[1].MemoryID != old {
			t.Fatal("since", since, b.Superseded)
		}
	}
	// Greater than the cap in each lower-priority bucket must not crowd out
	// local or workspace results. Both sides of each returned conflict are visible.
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	for i := 0; i < 55; i++ {
		gp := seed("global", "_global", fmt.Sprintf("global-p-%d", i), "global pending", "pending", "-100 days", "+1 day", "0 days")
		gs := seed("global", "_global", fmt.Sprintf("global-s-%d", i), "global superseded", "superseded", "-1 day", "", "0 days")
		conflict(gp, gs, "0 days", 0)
		hp := seed("project", "alerts-private", fmt.Sprintf("hidden-p-%d", i), "hidden pending", "pending", "-100 days", "+1 day", "0 days")
		hs := seed("project", "alerts-private", fmt.Sprintf("hidden-s-%d", i), "hidden superseded", "superseded", "-1 day", "", "0 days")
		conflict(hp, hs, "0 days", 0)
	}
	b, _ = run(nil)
	if len(b.Stale) != 50 || b.Stale[0].MemoryID != stale || len(b.Conflicts) != 50 || b.Conflicts[0].ConflictID != visibleConflict || len(b.Superseded) != 50 || b.Superseded[0].MemoryID != recent || b.Superseded[1].MemoryID != wsRecent {
		t.Fatal("scope before cap")
	}
	for _, r := range b.Conflicts {
		if r.ConflictID == hiddenConflict || r.ConflictID == resolvedConflict || strings.Contains(r.A, "hidden") || strings.Contains(r.B, "hidden") {
			t.Fatal(r)
		}
	}
	for _, r := range b.Stale {
		if strings.Contains(r.Text, "hidden") {
			t.Fatal(r)
		}
	}
	for _, r := range b.Superseded {
		if strings.Contains(r.Text, "hidden") {
			t.Fatal(r)
		}
	}
	// The plateau contract: after expiration, alerts cannot keep accumulating
	// pending commitments outside their lifetime. A repeated sweep is idempotent.
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,created_at,ttl_at)
 SELECT 'L2','fact','plateau-'||i,'commitment','project','alerts-project','pending',pg_now_text((-(90-i%90))||' days'),pg_now_text((-(90-i%90)+10)||' days') FROM generate_series(1,500) i`)
	sweep := func() int {
		t.Helper()
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{Operation: "lifecycle-sweep", Project: "alerts-project"}))
		var response DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(raw, &response) != nil || response.Count == nil {
			t.Fatal(status, string(raw))
		}
		return *response.Count
	}
	if n := sweep(); n < 400 {
		t.Fatal("expiration sweep", n)
	}
	b, _ = run(nil)
	var pending int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE lifecycle_state='pending' AND scope_value='alerts-project'`).Scan(&pending); err != nil {
		t.Fatal(err)
	}
	localAlerts := 0
	for _, r := range b.Stale {
		if r.MemoryID != workspace && r.Text != "global pending" {
			localAlerts++
		}
	}
	if pending >= 100 || localAlerts > pending || sweep() != 0 {
		t.Fatal("plateau", pending, localAlerts)
	}
	// A missing required table privilege must fail the whole bundle.
	exec(`SAVEPOINT alerts_failure; RESET ROLE`)
	exec(`REVOKE SELECT ON memory_conflicts FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	result := runPublicCommand(t, clientForHandler(t, handler), "alerts", `{"scope_context":true,"project":"alerts-project"}`)
	if result["kind"] != "unavailable" || result["alerts"] != nil {
		t.Fatal(result)
	}
	exec(`ROLLBACK TO SAVEPOINT alerts_failure; RELEASE SAVEPOINT alerts_failure`)
}

// Operator alerts retain pending and historical records, but neither side of a
// conflict may expose erased, quarantined or suppressed active content.
func TestAlertParentEligibilityPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL for packaged alert eligibility")
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	const key = "alert-parent-eligibility"
	exec(`DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
 CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$;
 GRANT USAGE ON SCHEMA public TO aimee_store_runtime;
 GRANT SELECT ON ALL TABLES IN SCHEMA public TO aimee_store_runtime;
 SELECT set_config('aimee.memory_scope_all','1',true)`)
	var a, b int64
	for _, id := range []*int64{&a, &b} {
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 VALUES('L2','fact',$1,'alert source','project',$1) RETURNING id`, key).Scan(id); err != nil {
			t.Fatal(err)
		}
	}
	exec(`INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES($1,$2,now()::text)`, a, b)
	backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
	for _, tc := range []struct {
		name, state string
		suppressed  int
		from, until string
		want        bool
	}{
		{"current", "active", 0, "", "", true}, {"future", "active", 0, "2999-01-01", "", true},
		{"expired", "active", 0, "", "2000-01-01", true}, {"suppressed", "active", 1, "", "", false},
		{"pending", "pending", 0, "", "", true}, {"fulfilled", "fulfilled", 0, "", "", true},
		{"superseded", "superseded", 1, "", "", true}, {"archived", "archived", 0, "", "", true},
		{"retired", "retired", 1, "", "", true}, {"quarantined", "quarantined", 0, "", "", false},
		{"deleted", "deleted", 0, "", "", false}, {"revoked", "revoked", 0, "", "", false},
		{"rejected", "rejected", 0, "", "", false}, {"unknown", "unknown", 0, "", "", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			for _, id := range []int64{a, b} {
				exec(`RESET ROLE`)
				exec(`UPDATE memories SET lifecycle_state='active',activation_suppressed=0,valid_from='',valid_until='' WHERE id IN ($1,$2)`, a, b)
				exec(`UPDATE memories SET lifecycle_state=$2,activation_suppressed=$3,valid_from=$4,valid_until=$5 WHERE id=$1`, id, tc.state, tc.suppressed, tc.from, tc.until)
				exec(`SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project',$1,true),
 set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value',$1,true)`, key)
				exec(`SET LOCAL ROLE aimee_store_runtime`)
				raw, err := backend.AlertsBundle(ctx, "")
				if err != nil {
					t.Fatal(err)
				}
				var got alertsBundle
				if err = json.Unmarshal(raw, &got); err != nil {
					t.Fatal(err)
				}
				if (len(got.Conflicts) == 1) != tc.want {
					t.Fatalf("parent %d: got %d conflicts; eligible=%v", id, len(got.Conflicts), tc.want)
				}
			}
		})
	}
	// Invalid conflicts newer than the eligible one must not consume its cap.
	exec(`RESET ROLE`)
	exec(`UPDATE memories SET lifecycle_state='active',activation_suppressed=0,valid_from='',valid_until='' WHERE id IN ($1,$2)`, a, b)
	exec(`WITH hidden AS (INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state)
 SELECT 'L2','fact',$1||i,'erased','project',$1,'deleted' FROM generate_series(1,60) i RETURNING id)
 INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) SELECT $2,id,'2999-01-01T00:00:00Z' FROM hidden`, key, a)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed,created_at,ttl_at)
 VALUES('L2','fact','suppressed-pending','hidden pending','project',$1,'pending',1,
 (now()-interval '9 days')::text,(now()+interval '1 day')::text)`, key)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	raw, err := backend.AlertsBundle(ctx, "")
	if err != nil {
		t.Fatal(err)
	}
	var got alertsBundle
	if err = json.Unmarshal(raw, &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Conflicts) != 1 || got.Conflicts[0].MemoryBID != b || len(got.Stale) != 0 {
		t.Fatalf("inspection backfill or suppressed pending: %+v", got)
	}

}
