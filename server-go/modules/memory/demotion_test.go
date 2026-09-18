package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestDemotionScore(t *testing.T) {
	now := time.Date(2026, 9, 18, 0, 0, 0, 0, time.UTC)
	for _, test := range []struct {
		name, payload, created string
		minimum                int
		halfLife, want         float64
		valid                  bool
	}{
		{"accepted", `{"verdict":"accepted"}`, "invalid", 1, 30, 1, true},
		{"corrected", `{"verdict":"corrected","weight":2}`, "invalid", 1, 30, -2, true},
		{"contradicted", `{"verdict":"contradicted"}`, "invalid", 1, 30, -1, true},
		{"rolled back", `{"verdict":"rolled_back"}`, "invalid", 1, 30, -1, true},
		{"irrelevant counts", `{"verdict":"irrelevant"}`, "invalid", 1, 30, 0, true},
		{"unknown counts", `{"verdict":"unknown"}`, "invalid", 1, 30, 0, true},
		{"nonnumeric weight defaults", `{"verdict":"accepted","weight":"2"}`, "invalid", 1, 30, 1, true},
		{"null weight defaults", `{"verdict":"accepted","weight":null}`, "invalid", 1, 30, 1, true},
		{"null verdict excluded", `{"verdict":null}`, "invalid", 1, 30, 0, false},
		{"missing verdict excluded", `{}`, "invalid", 1, 30, 0, false},
		{"overflow excluded", `{"verdict":"accepted","weight":1e400}`, "invalid", 1, 30, 0, false},
		{"half life RFC", `{"verdict":"accepted"}`, "2026-08-19T00:00:00Z", 1, 30, .5, true},
		{"half life SQL", `{"verdict":"accepted"}`, "2026-08-19 00:00:00", 1, 30, .5, true},
		{"future age zero", `{"verdict":"accepted"}`, "2027-08-19T00:00:00Z", 1, 30, 1, true},
		{"default half life", `{"verdict":"accepted"}`, "2026-08-19T00:00:00Z", 1, 0, .5, true},
		{"minimum", `{"verdict":"accepted"}`, "invalid", 2, 30, 1, false},
		{"default minimum", `{"verdict":"accepted"}`, "invalid", 0, 30, 1, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			score, valid := demotionScore([]demotionEvidence{{payload: json.RawMessage(test.payload), created: test.created}}, test.minimum, test.halfLife, now)
			if math.Abs(score-test.want) > 1e-10 || valid != test.valid {
				t.Fatal(score, valid)
			}
		})
	}
	rows := []demotionEvidence{}
	for _, verdict := range []string{"accepted", "corrected", "contradicted", "rolled_back", "irrelevant"} {
		rows = append(rows, demotionEvidence{payload: json.RawMessage(fmt.Sprintf(`{"verdict":%q}`, verdict))})
	}
	if score, valid := demotionScore(rows, 0, 0, now); !valid || score != -2 {
		t.Fatal(score, valid)
	}
}

func TestDemotionPercentiles(t *testing.T) {
	for _, test := range []struct {
		values  []float64
		p, want float64
	}{
		{nil, .1, 0}, {[]float64{7}, .1, 7}, {[]float64{-3, -1, 3}, .1, -2.6},
		{[]float64{-3, -1, 3}, .5, -1}, {[]float64{-3, -1, 3}, .9, 2.2},
		{[]float64{-3, -1, 3}, 0, -3}, {[]float64{-3, -1, 3}, 1, 3},
	} {
		if got := demotionPercentile(test.values, test.p); math.Abs(got-test.want) > 1e-10 {
			t.Fatal(test, got)
		}
	}
	// Admission must use the rounded profile even for a single score.
	for _, test := range []struct {
		score  float64
		demote bool
	}{{-.123456, false}, {-.123444, true}} {
		threshold := demotionRounded(demotionPercentile([]float64{test.score}, .1))
		if (test.score < threshold) != test.demote {
			t.Fatal(test, threshold)
		}
	}
}

func TestDemotionRuntimeAuthority(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"demotion-run","config":{"enabled":0}}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if placement == PlacementServer {
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
				t.Fatal(status)
			}
			continue
		}
		result := runHostRuntime(t, handler, `{"operation":"demotion-run","config":{"enabled":0}}`)
		if result["status"] != "ok" || result["profiles_written"] != float64(0) {
			t.Fatal(result)
		}
		for _, raw := range []string{`{}`, `{"config":null}`, `{"config":[]}`, `{"config":{"enabled":"live"}}`} {
			var args map[string]any
			json.Unmarshal([]byte(raw), &args)
			args["operation"] = "demotion-run"
			encoded, _ := json.Marshal(args)
			frame, _ := bus.EncodeCommand("runtime", encoded)
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(raw, status)
			}
		}
	}
}

func TestDemotionPreviewAuthority(t *testing.T) {
	frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"demotion-check","config":{"enabled":0}}`))
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		// Preview needs the store even when live demotion is disabled.
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(status)
		}
	}
}

func exerciseDemotionReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	execSQL := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	execSQL(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	kind := `demotion-"fixture`
	ids := []int64{}
	for n, weight := range []float64{-1, -1.0 / 3, 1} {
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,confidence) VALUES('L0',$1,$2,'demotion replay',0.8) RETURNING id`, kind, fmt.Sprintf("demotion-%d", n)).Scan(&id); err != nil {
			t.Fatal(err)
		}
		ids = append(ids, id)
		for j := 0; j < 3; j++ {
			execSQL(`INSERT INTO artifacts(id,kind,scope_kind,scope_id,created_at,payload)
    VALUES($1,'retrieval_attribution','memory',$2,'2099-01-01 00:00:00',$3::jsonb)`, fmt.Sprintf("demotion-replay-%d-%d", n, j), fmt.Sprint(id), fmt.Sprintf(`{"verdict":"accepted","weight":%g}`, weight))
		}
	}
	// Malformed IDs and missing memories must not alias valid candidates.
	for _, scope := range []string{"invalid", "0", "9223372036854775807", "+" + fmt.Sprint(ids[0]), "0" + fmt.Sprint(ids[0])} {
		for j := 0; j < 3; j++ {
			execSQL(`INSERT INTO artifacts(id,kind,scope_id,payload) VALUES($1,'retrieval_attribution',$2,'{"verdict":"accepted"}')`, fmt.Sprintf("demotion-invalid-%s-%d", scope, j), scope)
		}
	}
	run := func(mode int) map[string]any {
		t.Helper()
		return runHostRuntime(t, handler, fmt.Sprintf(`{"operation":"demotion-run","config":{"enabled":%d,"n_min":3,"window":3,"half_life_days":30}}`, mode))
	}
	assertConfidence := func(want float64) {
		t.Helper()
		var got float64
		if err := tx.QueryRow(ctx, `SELECT confidence FROM memories WHERE id=$1`, ids[0]).Scan(&got); err != nil || math.Abs(got-want) > 1e-10 {
			t.Fatal(got, want, err)
		}
	}
	counts := func() (int, int, int) {
		t.Helper()
		var profiles, actions, touched int
		if err := tx.QueryRow(ctx, `SELECT
  (SELECT count(*) FROM artifacts WHERE kind='demotion_profile' AND target_surface=$1),
  (SELECT count(*) FROM artifacts WHERE kind='demotion_action' AND payload->>'kind'=$1),
  (SELECT count(*) FROM artifacts WHERE id LIKE 'demotion-replay-%' AND last_accessed_at IS NOT NULL)`, kind).Scan(&profiles, &actions, &touched); err != nil {
			t.Fatal(err)
		}
		return profiles, actions, touched
	}
	if r := run(0); r["profiles_written"] != float64(0) {
		t.Fatal(r)
	}
	if p, a, touch := counts(); p != 0 || a != 0 || touch != 0 {
		t.Fatal(p, a, touch)
	}
	// A second host must skip instead of overlapping an in-flight run.
	other, err := pgx.Connect(ctx, os.Getenv("AIMEE_DB2_REPLAY_URL"))
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close(ctx)
	lock, err := other.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Rollback(ctx)
	if _, err := lock.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('memory:demotion-run',0))`); err != nil {
		t.Fatal(err)
	}
	if r := run(2); r["skipped"] != true || r["profiles_written"] != float64(0) {
		t.Fatal(r)
	}
	if p, a, touch := counts(); p != 0 || a != 0 || touch != 0 {
		t.Fatal(p, a, touch)
	}
	if err := lock.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if r := run(1); r["profiles_written"] != float64(1) || r["demoted"] != float64(0) || r["scored"] != float64(3) {
		t.Fatal(r)
	}
	assertConfidence(.8)
	if p, a, touch := counts(); p != 1 || a != 0 || touch != 9 {
		t.Fatal(p, a, touch)
	}
	if r := run(2); r["profiles_written"] != float64(2) || r["profiles_created"] != float64(1) || r["demoted"] != float64(1) {
		t.Fatal(r)
	}
	assertConfidence(.7)
	var profileValid, actionValid bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM artifacts WHERE kind='demotion_profile' AND target_surface=$1 AND state='committed' AND committed_at<>'' AND last_accessed_at IS NOT NULL AND payload->'score_percentiles'->>'p10'='-2.6'),
 EXISTS(SELECT 1 FROM artifacts WHERE kind='demotion_action' AND scope_id=$2 AND state='demoted' AND payload->>'kind'=$1 AND payload->>'score'='-3')`, kind, fmt.Sprint(ids[0])).Scan(&profileValid, &actionValid); err != nil || !profileValid || !actionValid {
		t.Fatal(profileValid, actionValid, err)
	}
	// Fail after the confidence UPDATE, proving the entire run rolls back.
	execSQL(`RESET ROLE;
 CREATE FUNCTION pg_temp.reject_demotion_action() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN
 IF NEW.kind='demotion_action' THEN RAISE EXCEPTION 'demotion action fixture'; END IF; RETURN NEW; END $$;
 CREATE TRIGGER reject_demotion_action BEFORE INSERT ON artifacts FOR EACH ROW EXECUTE FUNCTION pg_temp.reject_demotion_action();
 SET LOCAL ROLE aimee_store_runtime`)
	execSQL(`UPDATE artifacts SET last_accessed_at=NULL WHERE id LIKE 'demotion-replay-%'`)
	frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"demotion-run","config":{"enabled":2,"n_min":3,"window":3}}`))
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInternal {
		t.Fatal(status)
	}
	assertConfidence(.7)
	if p, a, touch := counts(); p != 2 || a != 1 || touch != 0 {
		t.Fatal("partial demotion committed", p, a, touch)
	}
	execSQL(`RESET ROLE; DROP TRIGGER reject_demotion_action ON artifacts; SET LOCAL ROLE aimee_store_runtime`)
	preview := func() map[string]any {
		t.Helper()
		return runHostRuntime(t, handler, `{"operation":"demotion-check","config":{"enabled":0,"n_min":3,"window":3}}`)
	}
	r := preview()
	if r["status"] != "ok" || r["demotion_enabled"] != float64(0) || r["scored"] != float64(3) || r["would_demote"] != float64(1) {
		t.Fatal(r)
	}
	assertConfidence(.7)
	if p, a, touch := counts(); p != 2 || a != 1 || touch != 9 {
		t.Fatal(p, a, touch)
	}
	// Preview uses the stored profile rather than fitting one from today's scores.
	execSQL(`INSERT INTO artifacts(id,kind,state,target_surface,scope_kind,committed_at,payload)
     VALUES('demotion-preview-override','demotion_profile','committed',$1,'global','2099-01-01','{"score_percentiles":{"p10":4}}')`, kind)
	r = preview()
	if r["would_demote"] != float64(3) {
		t.Fatal(r)
	}
	assertConfidence(.7)
	execSQL(`RESET ROLE; DELETE FROM artifacts WHERE kind='demotion_profile'; SET LOCAL ROLE aimee_store_runtime`)
	r = preview()
	if r["would_demote"] != float64(2) {
		t.Fatal("missing profile must use zero threshold", r)
	}
	if p, a, _ := counts(); p != 0 || a != 1 {
		t.Fatal("preview wrote an artifact", p, a)
	}

	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	readProfile := func(scope, id, want string) {
		t.Helper()
		raw, err := backend.demotionProfile(ctx, "fallback-fixture", scope, id)
		var got map[string]string
		if err != nil || json.Unmarshal(raw, &got) != nil || got["scope"] != want {
			t.Fatal(string(raw), want, err)
		}
	}
	writeProfile := func(id, scopeKind, scopeID, value string) {
		t.Helper()
		execSQL(`INSERT INTO artifacts(id,kind,state,scope_kind,scope_id,target_surface,committed_at,payload)
     VALUES($1,'demotion_profile','committed',$2,$3,'fallback-fixture',pg_now_text(),jsonb_build_object('scope',$4::text))`, id, scopeKind, scopeID, value)
	}
	writeProfile("demotion-fallback-global", "global", "", "global")
	readProfile("user", "alice", "global")
	writeProfile("demotion-fallback-kind", "user", "", "kind")
	readProfile("user", "alice", "kind")
	writeProfile("demotion-fallback-exact", "user", "alice", "exact")
	readProfile("user", "alice", "exact")
	readProfile("user", "bob", "kind")
	readProfile("project", "different", "global")
	if raw, err := backend.demotionProfile(ctx, "absent-class", "global", ""); err != nil || raw != nil {
		t.Fatal(string(raw), err)
	}
	var touched int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM artifacts WHERE id LIKE 'demotion-fallback-%' AND last_accessed_at IS NOT NULL`).Scan(&touched); err != nil || touched != 3 {
		t.Fatal(touched, err)
	}

}
