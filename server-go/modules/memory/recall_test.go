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

func TestRecallBudget(t *testing.T) {
	for _, limit := range []int{64, 96, 256, 600, 1800, 8192} {
		items := recallItems([]Record{{ID: math.MaxInt64, Scope: Scope{Type: ScopeGlobal, Value: "_global"}, Key: "identity:name", Content: strings.Repeat("界\"\n", 100)}})
		b := recallBundle{AlwaysOnRules: []recallRule{{ID: math.MaxInt64, Title: "Keep hard rules", Description: "Rule text"}}, Identity: items, Preferences: items, ActiveContext: items, OpenCommitments: items,
			Reminders: []recallReminder{{MemoryID: math.MaxInt64, Text: "Reminder"}}, Directives: []recallDirective{{MemoryID: math.MaxInt64, Text: "Directive"}}, LimitTokens: limit, Explain: []any{}}
		raw, err := b.encodeBudgeted()
		if err != nil || !json.Valid(raw) || b.ApproxTokens != (len(raw)+3)/4 || b.UsedTokens != b.ApproxTokens {
			t.Fatal(limit, string(raw), err)
		}
		lengths := []int{len(b.AlwaysOnRules), len(b.Identity), len(b.Preferences), len(b.ActiveContext), len(b.OpenCommitments), len(b.Reminders), len(b.Directives)}
		removed := false
		for _, n := range lengths {
			if removed && n > 0 {
				t.Fatal("lower priority survived", limit, lengths)
			}
			removed = removed || n == 0
		}
		if b.ApproxTokens > limit {
			if !b.BudgetExceeded {
				t.Fatal("unreported metadata overflow", limit)
			}
			for _, n := range lengths {
				if n != 0 {
					t.Fatal("nonempty overflow", limit)
				}
			}
		} else if b.BudgetExceeded {
			t.Fatal("spurious overflow", limit)
		}
		if len(b.Identity) > 0 && (!strings.Contains(string(raw), `"memory_id":9223372036854775807`) || b.Identity[0].Text != items[0].Text) {
			t.Fatal("truncated record")
		}
		var fields map[string]json.RawMessage
		_ = json.Unmarshal(raw, &fields)
		for _, section := range []string{"always_on_rules", "identity", "preferences", "active_context", "open_commitments", "reminders", "directives", "explain"} {
			if len(fields[section]) == 0 || fields[section][0] != '[' {
				t.Fatal("missing array", section, string(raw))
			}
		}
	}
	for _, c := range []struct {
		input int
		start bool
		want  int
	}{{0, false, 600}, {-1, true, 1800}, {1, false, 64}, {math.MaxInt32, false, 8192}} {
		if n := recallTokenLimit(c.input, c.start); n != c.want {
			t.Fatal(c, n)
		}
	}
}

// Exercise the packaged runtime role and RLS, with enough unrelated rows to
// crowd out a project unless scoping and priority happen before section LIMITs.
func exerciseRecallReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT recall_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT recall_replay; RELEASE SAVEPOINT recall_replay`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`UPDATE memories SET activation_suppressed=1; UPDATE epistemic_directives SET state='suppressed'; UPDATE prospective_memories SET state='cancelled'; UPDATE rules SET directive_type='soft'`)
	seed := func(tier, kind, key, scope, value, state string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,confidence,scope_type,scope_value,lifecycle_state)
 VALUES($1,$2,$3,$3,0.1,$4,$5,$6) RETURNING id`, tier, kind, key, scope, value, state).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	identity := seed("L3", "fact", "identity:name", "project", "recall-project", "active")
	role := seed("L2", "fact", "role:engineer", "project", "recall-project", "active")
	self := seed("L2", "fact", "self:timezone", "project", "recall-project", "active")
	seed("L2", "fact", "project:role-name-identity", "project", "recall-project", "active")
	seed("L1", "fact", "identity:low", "project", "recall-project", "active")
	pref := seed("L2", "preference", "pref:indent", "project", "recall-project", "active")
	commit := seed("L2", "fact", "commit:review", "project", "recall-project", "pending")
	secret := seed("L3", "fact", "identity:unrelated", "project", "private-project", "active")
	for i := 0; i < 40; i++ {
		global := seed("L3", "fact", fmt.Sprintf("identity:global-%d", i), "global", "_global", "active")
		private := seed("L3", "fact", fmt.Sprintf("identity:private-%d", i), "project", "private-project", "active")
		exec(`UPDATE memories SET confidence=1 WHERE id=ANY($1::bigint[])`, []int64{global, private})
	}
	// High-ranked inapplicable rows exceed every section cap. Selection must
	// backfill from current rows, including pending commitments and activation.
	for _, state := range []string{"active", "pending"} {
		for _, kind := range []string{"fact", "preference"} {
			for i := 0; i < 12; i++ {
				for _, endpoint := range []string{"future", "expired"} {
					id := seed("L3", kind, fmt.Sprintf("identity:inapplicable-%s-%s-%s-%d backend migration", state, kind, endpoint, i), "project", "recall-project", state)
					if endpoint == "future" {
						exec(`UPDATE memories SET confidence=1,valid_from=(CURRENT_TIMESTAMP+interval '1 day')::text WHERE id=$1`, id)
					} else {
						exec(`UPDATE memories SET confidence=1,valid_until=CURRENT_TIMESTAMP::text WHERE id=$1`, id)
					}
				}
			}
		}
	}
	exec(`INSERT INTO rules(polarity,title,description,weight,directive_type,created_at,updated_at)
 VALUES('negative','Recall hard rule','Do not expose secrets',10,'hard',pg_now_text(),pg_now_text()),
 ('positive','Recall soft rule','Optional advice',99,'soft',pg_now_text(),pg_now_text())`)
	var directive int64
	if err := tx.QueryRow(ctx, `INSERT INTO epistemic_directives(question,topic,cause,priority,state)
 VALUES('Which backend should we choose?','backend migration','user_follow_up',99,'open') RETURNING id`).Scan(&directive); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO epistemic_directives(question,topic,cause,priority,state,valid_until)
 VALUES('Expired question','backend migration','user_follow_up',100,'open',pg_now_text('-1 day'))`)
	exec(`INSERT INTO prospective_memories(trigger_text,action_text,state) VALUES('backend migration','Remember the rollback plan','armed')`)
	client := clientForHandler(t, handler)
	recall := func(query string, limit int, start bool, activation string) (recallBundle, []byte) {
		t.Helper()
		args := fmt.Sprintf(`{"task_hint":%q,"limit_tokens":%d,"session_start":%t,"scope_context":true,"project":"recall-project"%s}`, query, limit, start, activation)
		raw, err := client.Command(ctx, 73, "recall", json.RawMessage(args))
		if err != nil {
			t.Fatal(err)
		}
		var envelope struct {
			Status string
			Recall json.RawMessage
		}
		if json.Unmarshal(raw, &envelope) != nil || envelope.Status != "ok" {
			t.Fatalf("recall: %s", raw)
		}
		var b recallBundle
		if err := json.Unmarshal(envelope.Recall, &b); err != nil {
			t.Fatal(err)
		}
		if b.ApproxTokens != (len(envelope.Recall)+3)/4 {
			t.Fatal("wrong estimate", b.ApproxTokens, len(envelope.Recall))
		}
		if strings.Contains(string(raw), "inapplicable-") {
			t.Fatal("inapplicable memory escaped bundle gate", string(raw))
		}
		if strings.Contains(string(raw), "private-project") {
			t.Fatal("private recall row")
		}
		return b, envelope.Recall
	}
	b, _ := recall("backend migration", 8192, false, "")
	if len(b.Identity) != 3 || b.Identity[0].ID != self || b.Identity[1].ID != role || b.Identity[2].ID != identity {
		t.Fatalf("identity prefixes/scope: %+v", b.Identity)
	}
	if len(b.Preferences) != 1 || b.Preferences[0].ID != pref || b.Preferences[0].Why != "stable preference" {
		t.Fatal(b.Preferences)
	}
	if len(b.OpenCommitments) != 1 || b.OpenCommitments[0].ID != commit {
		t.Fatal(b.OpenCommitments)
	}
	if len(b.AlwaysOnRules) != 1 || b.AlwaysOnRules[0].Title != "Recall hard rule" {
		t.Fatal(b.AlwaysOnRules)
	}
	if len(b.Directives) != 1 || b.Directives[0].ID != directive || b.Directives[0].MemoryID != directive || b.Directives[0].Kind != "directive" || b.Directives[0].Why != "directive:user_follow_up" || b.Directives[0].Text != b.Directives[0].Question {
		t.Fatal(b.Directives)
	}
	if len(b.Reminders) != 1 || b.Reminders[0].Kind != "reminder" || b.Reminders[0].Text != "Remember the rollback plan" || b.Reminders[0].Why == "" {
		t.Fatal(b.Reminders)
	}
	for _, r := range b.Identity {
		if r.Why == "" || r.ID == secret {
			t.Fatal(r)
		}
	}
	var surfaced int
	count := func() int {
		t.Helper()
		if err := tx.QueryRow(ctx, `SELECT surfaced_count FROM epistemic_directives WHERE id=$1`, directive).Scan(&surfaced); err != nil {
			t.Fatal(err)
		}
		return surfaced
	}
	if count() != 1 {
		t.Fatal("surfaced counter", surfaced)
	}
	// Session-start and a nonmatching hint fall back to unexpired open directives.
	for _, query := range []string{"", "unrelated hint"} {
		session, _ := recall(query, 8192, true, "")
		if len(session.Identity) != 6 || len(session.Directives) != 1 || session.Directives[0].ID != directive {
			t.Fatal(session)
		}
	}
	before := count()
	tiny, _ := recall("backend migration", 64, false, "")
	if len(tiny.Directives) != 0 || len(tiny.Identity) != 0 || tiny.BudgetExceeded != (tiny.ApproxTokens > tiny.LimitTokens) || count() != before {
		t.Fatal("trimmed directive was counted", tiny, surfaced)
	}
	// The default and clamped budgets remain truthful, including all aliases.
	for _, limit := range []int{0, 96, 600, math.MaxInt32} {
		r, _ := recall("backend migration", limit, false, "")
		if r.LimitTokens != recallTokenLimit(limit, false) || r.ApproxTokens > r.LimitTokens && !r.BudgetExceeded {
			t.Fatal(r)
		}
	}
	// Activation still gates before LIMIT and preserves scope ordering.
	activated, _ := recall("backend migration", 8192, false, fmt.Sprintf(`,"activation":{"current_turn":2,"rows":[{"memory_id":%d,"last_turn":1}]}`, self))
	if len(activated.Identity) != 3 || activated.Identity[0].ID != role || !activated.Identity[0].ActivationManaged {
		t.Fatal(activated.Identity)
	}
	// A directive write failure rolls back earlier surfaced counters as well.
	before = count()
	exec(`SAVEPOINT recall_write_denied; RESET ROLE;
 INSERT INTO epistemic_directives(question,topic,cause,priority,state)
 VALUES('Rejected surfaced write','backend migration','user_follow_up',98,'open');
 ALTER TABLE epistemic_directives ADD CONSTRAINT recall_surface_failure
 CHECK(question<>'Rejected surfaced write' OR surfaced_count=0);
 SET LOCAL ROLE aimee_store_runtime`)
	failed := runPublicCommand(t, client, "recall", `{"task_hint":"backend migration","limit_tokens":8192,"scope_context":true,"project":"recall-project"}`)
	if failed["kind"] != "unavailable" || failed["recall"] != nil || count() != before {
		t.Fatal("partial recall write", failed, surfaced)
	}
	exec(`ROLLBACK TO SAVEPOINT recall_write_denied; RELEASE SAVEPOINT recall_write_denied`)
	// A required rules read failure cannot turn into an apparently empty success.
	exec(`SAVEPOINT recall_denied; RESET ROLE; REVOKE SELECT ON rules FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	result := runPublicCommand(t, client, "recall", `{"scope_context":true,"project":"recall-project"}`)
	if result["kind"] != "unavailable" || result["recall"] != nil {
		t.Fatal(result)
	}
	exec(`ROLLBACK TO SAVEPOINT recall_denied; RELEASE SAVEPOINT recall_denied`)
}
