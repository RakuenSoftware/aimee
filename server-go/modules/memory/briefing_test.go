package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestBriefingBudget(t *testing.T) {
	for _, limit := range []int{64, 300, 1024, 8192} {
		b := briefingBundle{Facts: []briefingFact{}, Activity: []briefingActivity{}, Entities: []briefingEntity{}, Style: "compact", BriefingStyle: "compact", LimitTokens: limit}
		for i := 0; i < 30; i++ {
			b.Facts = append(b.Facts, briefingFact{RecallRecord: recallItems([]Record{{ID: math.MaxInt64, Key: fmt.Sprint(i), Content: strings.Repeat("界", 30)}})[0]})
			b.Activity = append(b.Activity, briefingActivity{SessionID: fmt.Sprint(i), Summary: strings.Repeat("episode", 20)})
			b.Entities = append(b.Entities, briefingEntity{Name: fmt.Sprint(i), Mentions: i})
		}
		raw, err := b.encodeBudgeted()
		if err != nil || !json.Valid(raw) || (len(raw)+3)/4 > limit || b.ApproxTokens != (len(raw)+3)/4 {
			t.Fatal(limit, len(raw), err)
		}
		if len(b.Facts) < 30 && (len(b.Activity) > 0 || len(b.Entities) > 0) || len(b.Activity) < 30 && len(b.Entities) > 0 {
			t.Fatal("priority", limit)
		}
		if len(b.Facts) > 0 && !strings.Contains(string(raw), `"memory_id":9223372036854775807`) {
			t.Fatal("ID lost", string(raw))
		}
	}
}

// The packaged NOBYPASSRLS runtime exercises the old native briefing and
// scope-before-LIMIT assertions, including persisted policy selection.
func exerciseBriefingReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT briefing_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT briefing_replay; RELEASE SAVEPOINT briefing_replay`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	// Isolate this bundle from other replay seeds without changing their state.
	exec(`UPDATE memories SET activation_suppressed=1`)
	seed := func(tier, kind, key, scope, value, state, sensitivity string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,sensitivity,confidence)
 VALUES($1,$2,$3,$3,$4,$5,$6,$7,0.1) RETURNING id`, tier, kind, key, scope, value, state, sensitivity).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	local := seed("L3", "fact", "brief:local", "project", "brief-project", "active", "normal")
	mid := seed("L2", "fact", "brief:mid", "project", "brief-project", "active", "normal")
	seed("L1", "fact", "brief:low", "project", "brief-project", "active", "normal")
	seed("L2", "scratch", "brief:scratch", "project", "brief-project", "active", "normal")
	secret := seed("L2", "fact", "brief:secret", "project", "brief-project", "active", "secret")
	archived := seed("L2", "fact", "brief:archived", "project", "brief-project", "archived", "normal")
	suppressed := seed("L2", "fact", "brief:suppressed", "project", "brief-project", "active", "normal")
	private := seed("L2", "fact", "brief:private", "project", "brief-private", "active", "normal")
	exec(`UPDATE memories SET evidence_strength=0.9,observation_count=20,use_count=7 WHERE id=$1`, local)
	exec(`UPDATE memories SET activation_suppressed=1 WHERE id=$1`, suppressed)
	episode := func(id int64, session, text, date string) {
		t.Helper()
		exec(`INSERT INTO memory_episodes(memory_id,episode_key,source_session,episode_text,reference_time,created_at) VALUES($1,$2,$3,$2,$4,$4)`, id, text, session, date)
	}
	episode(local, "sess-a", "local episode", "2026-04-15T10:00:00Z")
	episode(mid, "sess-b", "mid episode", "2026-04-16T10:00:00Z")
	episode(mid, "sess-a", "older duplicate", "2026-04-10T10:00:00Z")
	episode(mid, "", "anonymous episode", "2026-04-17T10:00:00Z")
	for _, id := range []int64{private, secret, archived, suppressed} {
		episode(id, fmt.Sprint(id), "excluded-"+fmt.Sprint(id), "2026-09-17T10:00:00Z")
		exec(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,$2)`, id, "excluded-"+fmt.Sprint(id))
	}
	// Invalid parents cannot supply facts, episode summaries or entity counts,
	// even when enough high-ranked rows exist to fill the section limits.
	for i := 0; i < 65; i++ {
		id := seed("L3", "fact", fmt.Sprintf("brief:inapplicable-%d", i), "project", "brief-project", "active", "normal")
		if i%2 == 0 {
			exec(`UPDATE memories SET confidence=1,valid_from=(CURRENT_TIMESTAMP+interval '1 day')::text WHERE id=$1`, id)
		} else {
			exec(`UPDATE memories SET confidence=1,valid_until=CURRENT_TIMESTAMP::text WHERE id=$1`, id)
		}
		episode(id, fmt.Sprint(id), "inapplicable-"+fmt.Sprint(id), "2099-09-17T10:00:00Z")
		exec(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,'caroline'),($1,$2)`, id, "inapplicable-"+fmt.Sprint(id))
	}
	exec(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,'caroline'),($2,'caroline'),($2,'atlas')`, local, mid)
	promote := func(style string) {
		t.Helper()
		exec(`RESET ROLE`)
		exec(`INSERT INTO bandit_promotions(decision_point,arm_id,rollback_arm) VALUES('briefing_style',$1,'compact') ON CONFLICT(decision_point) DO UPDATE SET arm_id=EXCLUDED.arm_id`, style)
		exec(`SET LOCAL ROLE aimee_store_runtime`)
	}
	promote("compact")
	run := func(limit int) (briefingBundle, []byte) {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"scope_context": true, "project": "brief-project", "workspace": "brief-workspace", "limit_tokens": limit})
		frame, _ := bus.EncodeCommand("briefing", args)
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		raw, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var envelope struct {
			Status   string          `json:"status"`
			Briefing json.RawMessage `json:"briefing"`
		}
		if json.Unmarshal(raw, &envelope) != nil || envelope.Status != "ok" {
			t.Fatal(string(raw))
		}
		var b briefingBundle
		if json.Unmarshal(envelope.Briefing, &b) != nil {
			t.Fatal(string(raw))
		}
		if b.ApproxTokens != (len(envelope.Briefing)+3)/4 || b.ApproxTokens > b.LimitTokens {
			t.Fatal("budget", string(raw))
		}
		return b, envelope.Briefing
	}
	b, raw := run(0)
	if b.BriefingStyle != "compact" || b.LimitTokens != 1024 || len(b.Facts) != 2 || b.Facts[0].ID != local || b.Facts[1].ID != mid || b.Facts[0].MemoryID != local || b.Facts[0].Text != "brief:local" || b.Facts[0].ObservationCount != 20 {
		t.Fatal(b)
	}
	if len(b.Activity) != 2 || b.Activity[0].SessionID != "sess-b" || b.Activity[1].Summary != "local episode" {
		t.Fatal(b.Activity)
	}
	if len(b.Entities) != 2 || b.Entities[0].Name != "caroline" || b.Entities[0].Mentions != 2 {
		t.Fatal(b.Entities)
	}
	exerciseBriefingViewsReplay(t, handler, b)
	_, again := run(0)
	if string(raw) != string(again) {
		t.Fatal("not deterministic")
	}
	b, _ = run(64)
	if len(b.Activity) != 0 || len(b.Entities) != 0 {
		t.Fatal(b)
	}
	promote("evidence_heavy")
	b, _ = run(0)
	if b.LimitTokens != 3000 || b.BriefingStyle != "evidence_heavy" {
		t.Fatal(b)
	}
	b, _ = run(99999)
	if b.LimitTokens != 8192 {
		t.Fatal(b)
	}
	promote("unknown")
	b, _ = run(0)
	if b.LimitTokens != 1024 || b.BriefingStyle != "compact" {
		t.Fatal(b)
	}
	// Every result section must select local evidence ahead of more recent and
	// more numerous global/private distractors, before its own SQL row cap.
	promote("evidence_heavy")
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	for i := 0; i < 65; i++ {
		global := seed("L2", "fact", fmt.Sprintf("brief:global-%d", i), "global", "_global", "active", "normal")
		exec(`UPDATE memories SET confidence=1,evidence_strength=1,observation_count=100 WHERE id=$1`, global)
		episode(global, fmt.Sprintf("global-%02d", i), "global episode", "2026-09-18T10:00:00Z")
		exec(`INSERT INTO memory_entities(memory_id,entity) VALUES($1,'global-entity')`, global)
		if i == 0 {
			episode(global, "sess-a", "newer global duplicate", "2026-09-18T10:00:00Z")
		}
	}
	b, _ = run(8192)
	if len(b.Facts) == 0 || b.Facts[0].ID != local {
		t.Fatal("fact scope priority", b)
	}
	// Hold facts out so lower-priority sections are not intentionally budget-trimmed.
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`UPDATE memories SET tier='L1' WHERE key LIKE 'brief:%'`)
	b, _ = run(8192)
	if len(b.Activity) != 10 || b.Activity[0].SessionID != "sess-b" || b.Activity[1].Summary != "local episode" {
		t.Fatal("activity scope priority", b.Activity)
	}
	if len(b.Entities) < 3 || b.Entities[0].Name != "caroline" || b.Entities[1].Name != "atlas" {
		t.Fatal("entity scope priority", b.Entities)
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`UPDATE memories SET last_used_at=pg_now_text('-31 days') WHERE id=$1`, mid)
	b, _ = run(8192)
	for _, e := range b.Entities {
		if e.Name == "atlas" || e.Name == "caroline" && e.Mentions != 1 {
			t.Fatal("stale entity", b.Entities)
		}
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	fullText := strings.Repeat("界", 900) + " final-marker"
	exec(`UPDATE memories SET tier='L3',content=$2 WHERE id=$1`, local, fullText)
	b, _ = run(8192)
	if len(b.Facts) != 1 || b.Facts[0].Content != fullText || b.Facts[0].Text != fullText {
		t.Fatal("full text lost")
	}
	// Failure of a required section is unavailable, never an empty success.
	exec(`SAVEPOINT briefing_failure; RESET ROLE`)
	exec(`REVOKE SELECT ON memory_entities FROM aimee_store_runtime`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	result := runPublicCommand(t, clientForHandler(t, handler), "briefing", `{"scope_context":true,"project":"brief-project"}`)
	if result["kind"] != "unavailable" || result["briefing"] != nil {
		t.Fatal(result)
	}
	exec(`ROLLBACK TO SAVEPOINT briefing_failure; RELEASE SAVEPOINT briefing_failure`)

}
