package families

import (
	"context"
	"encoding/json"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"strings"
	"sync"
	"testing"
	"time"
)

const planUUID = "32345678-1234-4234-8234-123456789abc"

func planDescriptor(text, layout string) (string, string) {
	wire, _ := json.Marshal(map[string]any{"schema_version": 1, "safe_text_sha256": deliveryHash(text), "layout": layout})
	return string(wire), deliveryHash(string(wire))
}
func reservePlan(t *testing.T, db ownedInputDB, sid, actor, text, layout string) inputPlanRecord {
	t.Helper()
	opaque, digest := planDescriptor(text, layout)
	cells := ownedInputExpect(t, db, store.StatusOK, 63, sid, planUUID, actor, "plan-owner", deliveryHash(text), digest, opaque)
	var p inputPlanRecord
	if len(cells) != 1 || json.Unmarshal([]byte(cells[0]), &p) != nil {
		t.Fatal("bad plan receipt")
	}
	return p
}
func plannedBegin(t *testing.T, db ownedInputDB, want uint32, p inputPlanRecord, turn, text string) []string {
	t.Helper()
	return ownedInputExpect(t, db, want, 64, p.Session, p.Operation, turn, p.Actor, ownedInputBatch(p.Operation, text, "safe opaque"), func() string {
		b, _ := json.Marshal(inputDeliveryIntent{1, p.Receiver, deliveryHash("submitted"), "signed-submitted"})
		return string(b)
	}(), p.ID, p.Hash)
}
func probePlan(t *testing.T, db ownedInputDB, sid, actor string) string {
	t.Helper()
	cells := ownedInputExpect(t, db, store.StatusOK, 62, sid, planUUID, actor)
	var p struct {
		Presence string `json:"presence"`
	}
	if len(cells) != 1 || json.Unmarshal([]byte(cells[0]), &p) != nil {
		t.Fatal("bad probe")
	}
	return p.Presence
}
func TestOwnedPlanLifecycle(t *testing.T) {
	db, p := deliveryFixture(t)
	ctx := context.Background()
	for _, sid := range []string{"plan-a", "plan-old56", "plan-old58", "plan-terminal", "plan-owner-other", "plan-expired-pending", "plan-expired-terminal"} {
		actor := "alice"
		if sid == "plan-owner-other" {
			actor = "bob"
		}
		if _, err := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,$2)`, sid, actor); err != nil {
			t.Fatal(err)
		}
	}
	ownedInputExpect(t, db, store.StatusMissing, 62, "missing-session", planUUID, "alice")
	ownedInputExpect(t, db, store.StatusInvalid, 62, "plan-owner-other", planUUID, "alice")
	if probePlan(t, db, "plan-a", "alice") != "owned_absent" {
		t.Fatal("owned absence")
	}
	text := "safe normalized input"
	batch := ownedInputBatch(planUUID, text, "safe opaque")
	intent := deliveryIntent(deliveryHash("submitted"), "signed-submitted")
	ownedInputExpect(t, db, store.StatusOK, 56, "plan-old56", planUUID, "old-turn", "alice", batch)
	ownedInputExpect(t, db, store.StatusOK, 58, "plan-old58", planUUID, "old-turn", "alice", batch, intent)
	if probePlan(t, db, "plan-old56", "alice") != "legacy" || probePlan(t, db, "plan-old58", "alice") != "delivery" {
		t.Fatal("legacy/delivery probe")
	}
	opaque, digest := planDescriptor(text, "zero")
	for _, sid := range []string{"plan-old56", "plan-old58"} {
		ownedInputExpect(t, db, store.StatusConflict, 63, sid, planUUID, "alice", "plan-owner", deliveryHash(text), digest, opaque)
	}
	r := reservePlan(t, db, "plan-a", "alice", text, "zero")
	again := reservePlan(t, db, "plan-a", "alice", text, "zero")
	if r.ID != again.ID || r.Hash != again.Hash || r.State != "reserved" || probePlan(t, db, "plan-a", "alice") != "reserved" {
		t.Fatal("reservation replay")
	}
	changed, changedHash := planDescriptor(text, "one-span")
	ownedInputExpect(t, db, store.StatusConflict, 63, "plan-a", planUUID, "alice", "plan-owner", deliveryHash(text), changedHash, changed)
	// Both legacy paths must roll back after the deferred same-op constraint.
	ownedInputExpect(t, db, store.StatusFailed, 56, "plan-a", planUUID, "unplanned56", "alice", batch)
	ownedInputExpect(t, db, store.StatusFailed, 58, "plan-a", planUUID, "unplanned58", "alice", batch, intent)
	if ownedInputCount(t, p, "session_ir_events", "plan-a") != 0 || ownedInputCount(t, p, "session_input_deliveries", "plan-a") != 0 {
		t.Fatal("unplanned path bypassed reservation")
	}
	wrong := r
	wrong.Hash = deliveryHash("wrong")
	plannedBegin(t, db, store.StatusConflict, wrong, "wrong-plan", text)
	wrong = r
	wrong.Actor = "bob"
	plannedBegin(t, db, store.StatusInvalid, wrong, "wrong-owner", text)
	plannedBegin(t, db, store.StatusConflict, r, "wrong-text", "different safe text")
	c := plannedBegin(t, db, store.StatusOK, r, "first-planned-turn", text)
	if len(c) != 6 || c[4] != "created" || probePlan(t, db, "plan-a", "alice") != "committed" {
		t.Fatal("planned atomic commit")
	}
	replay := plannedBegin(t, db, store.StatusOK, r, "changed-caller-turn", text)
	if replay[4] != "replay" || replay[2] != "first-planned-turn" || replay[5] != c[5] {
		t.Fatal("planned replay changed tuple")
	}
	ownedInputExpect(t, db, store.StatusConflict, 65, r.ID, r.Session, r.Operation, r.Actor, r.Receiver, r.Hash, `{"fixture_verified_closed":true}`)
	ownedInputExpect(t, db, store.StatusConflict, opServerSessionDelete, r.Session)
	ownedInputExpect(t, db, store.StatusOK, 61, c[5], r.Receiver, r.Actor, deliveryHash("submitted"), "delivered", `{"fixture_delivery":true}`)
	ownedInputExpect(t, db, store.StatusOK, opServerSessionDelete, r.Session)
	// Terminal result here is an explicit fixture stand-in, not Native proof.
	term := reservePlan(t, db, "plan-terminal", "alice", text, "one-span")
	ownedInputExpect(t, db, store.StatusConflict, opServerSessionDelete, term.Session)
	ownedInputExpect(t, db, store.StatusInvalid, 65, term.ID, term.Session, term.Operation, "bob", term.Receiver, term.Hash, `{"fixture_verified_closed":true}`)
	ownedInputExpect(t, db, store.StatusConflict, 65, term.ID, term.Session, term.Operation, term.Actor, term.Receiver, deliveryHash("wrong"), `{"fixture_verified_closed":true}`)
	result := `{"fixture_verified_closed":true,"wide":9007199254740993}`
	ownedInputExpect(t, db, store.StatusOK, 65, term.ID, term.Session, term.Operation, term.Actor, term.Receiver, term.Hash, result)
	ownedInputExpect(t, db, store.StatusOK, 65, term.ID, term.Session, term.Operation, term.Actor, term.Receiver, term.Hash, result)
	ownedInputExpect(t, db, store.StatusConflict, 65, term.ID, term.Session, term.Operation, term.Actor, term.Receiver, term.Hash, `{"changed":true}`)
	if probePlan(t, db, term.Session, "alice") != "terminal" {
		t.Fatal("terminal probe")
	}
	plannedBegin(t, db, store.StatusConflict, term, "after-terminal", text)
	ownedInputExpect(t, db, store.StatusFailed, 56, term.Session, planUUID, "after-terminal56", "alice", batch)
	ownedInputExpect(t, db, store.StatusFailed, 58, term.Session, planUUID, "after-terminal58", "alice", batch, intent)
	ownedInputExpect(t, db, store.StatusOK, opServerSessionDelete, term.Session)
	for _, sid := range []string{"plan-expired-pending", "plan-expired-terminal"} {
		r := reservePlan(t, db, sid, "alice", text, "zero")
		if sid == "plan-expired-terminal" {
			ownedInputExpect(t, db, store.StatusOK, 65, r.ID, r.Session, r.Operation, r.Actor, r.Receiver, r.Hash, result)
		}
		if _, err := p.Exec(ctx, `UPDATE server_sessions SET created_at=now()-interval '365 days' WHERE id=$1`, sid); err != nil {
			t.Fatal(err)
		}
	}
	expired := ownedInputExpect(t, db, store.StatusOK, opServerSessionDeleteExpired, "86400")
	if len(expired) != 1 || expired[0] != "1" || ownedInputCount(t, p, "session_input_plans", "plan-expired-pending") != 1 || ownedInputCount(t, p, "session_input_plans", "plan-expired-terminal") != 0 {
		t.Fatal("reservation retention/expired skip", expired)
	}
	t.Log("PASS owned probe vs missing/other/legacy; immutable reserve; unplanned56/58 fence; atomic64/replay/bindings; pending retention/terminal65/deletion; terminal legacy bypass denied. No Raw2/Native fence proof claimed.")
}
func TestOwnedPlanRaces(t *testing.T) {
	db, p := deliveryFixture(t)
	ctx := context.Background()
	for _, sid := range []string{"plan-race-layout", "plan-race-same", "plan-64-first", "plan-65-first", "plan-first56", "plan-first58"} {
		if _, err := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'alice')`, sid); err != nil {
			t.Fatal(err)
		}
	}
	text := "safe raced normalized input"
	var wg sync.WaitGroup
	statuses := make([]uint32, 8)
	responses := make([][]string, 8)
	errs := make([]error, 8)
	for i := range statuses {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			op, hash := planDescriptor(text, fmt.Sprintf("layout-%d", i))
			statuses[i], responses[i], errs[i] = ownedInputCall(db, 63, "plan-race-layout", planUUID, "alice", "plan-owner", deliveryHash(text), hash, op)
		}(i)
	}
	wg.Wait()
	wins := 0
	for i, s := range statuses {
		if errs[i] != nil {
			t.Fatal(errs[i])
		}
		if s == store.StatusOK {
			wins++
		} else if s != store.StatusConflict {
			t.Fatal("unexpected race status", s)
		}
	}
	if wins != 1 || ownedInputCount(t, p, "session_input_plans", "plan-race-layout") != 1 {
		t.Fatal("different-layout race", wins)
	}
	op, hash := planDescriptor(text, "same")
	for i := range statuses {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			statuses[i], responses[i], errs[i] = ownedInputCall(db, 63, "plan-race-same", planUUID, "alice", "plan-owner", deliveryHash(text), hash, op)
		}(i)
	}
	wg.Wait()
	var id string
	for i, s := range statuses {
		if errs[i] != nil || s != store.StatusOK {
			t.Fatal("same-plan race", s, errs[i])
		}
		var r inputPlanRecord
		if json.Unmarshal([]byte(responses[i][0]), &r) != nil {
			t.Fatal("receipt")
		}
		if id == "" {
			id = r.ID
		}
		if r.ID != id {
			t.Fatal("duplicate plan IDs")
		}
	}
	// Deterministic both-order terminal/commit cases. Parent lock serializes them.
	first := reservePlan(t, db, "plan-64-first", "alice", text, "zero")
	plannedBegin(t, db, store.StatusOK, first, "commit-first", text)
	ownedInputExpect(t, db, store.StatusConflict, 65, first.ID, first.Session, first.Operation, first.Actor, first.Receiver, first.Hash, `{"fixture_verified_closed":true}`)
	terminal := reservePlan(t, db, "plan-65-first", "alice", text, "zero")
	ownedInputExpect(t, db, store.StatusOK, 65, terminal.ID, terminal.Session, terminal.Operation, terminal.Actor, terminal.Receiver, terminal.Hash, `{"fixture_verified_closed":true}`)
	plannedBegin(t, db, store.StatusConflict, terminal, "terminal-first", text)
	// Legacy wins first -> reserve must reject before any future custody call.
	batch := ownedInputBatch(planUUID, text, "safe opaque")
	ownedInputExpect(t, db, store.StatusOK, 56, "plan-first56", planUUID, "legacy-first", "alice", batch)
	ownedInputExpect(t, db, store.StatusOK, 58, "plan-first58", planUUID, "legacy-first", "alice", batch, deliveryIntent(deliveryHash("submitted"), "signed"))
	for _, sid := range []string{"plan-first56", "plan-first58"} {
		ownedInputExpect(t, db, store.StatusConflict, 63, sid, planUUID, "alice", "plan-owner", deliveryHash(text), hash, op)
	}
	var n int
	if err := p.QueryRow(ctx, `SELECT count(*) FROM session_input_plans WHERE session_id IN ('plan-first56','plan-first58')`).Scan(&n); err != nil || n != 0 {
		t.Fatal("legacy first created plan", n, err)
	}
	t.Log("PASS concurrent different/same layouts, both64/65 outcomes, legacy-first56/58 reject reservation; reserve-first56/58 covered lifecycle. Pending plans retain no raw selected values.")
}

func TestOwnedPlanLockOrders(t *testing.T) {
	db, p := deliveryFixture(t)
	ctx := context.Background()
	text := "safe lock-ordered input"
	opaque, digest := planDescriptor(text, "zero")
	batch := ownedInputBatch(planUUID, text, "safe opaque")
	delivery := deliveryIntent(deliveryHash("submitted"), "signed")
	type answer struct {
		status uint32
		cells  []string
		err    error
	}
	for _, legacy := range []uint32{56, 58} {
		for _, reserveFirst := range []bool{true, false} {
			sid := fmt.Sprintf("plan-locked-%d-%t", legacy, reserveFirst)
			if _, e := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'alice')`, sid); e != nil {
				t.Fatal(e)
			}
			tx, e := p.Begin(ctx)
			if e != nil {
				t.Fatal(e)
			}
			q := ownedDeliveryTx{sessionIRLiveTx{tx}}
			args := []string{sid, planUUID, "legacy-turn", "alice", batch}
			if legacy == 58 {
				args = append(args, delivery)
			}
			var status uint32
			if reserveFirst {
				status, _, e = sessionInputPlanReserve(ctx, q, []string{sid, planUUID, "alice", "plan-owner", deliveryHash(text), digest, opaque})
			} else if legacy == 56 {
				status, _, e = sessionInputAttachmentAppend(ctx, q, args)
			} else {
				status, _, e = sessionInputDeliveryBegin(ctx, q, args)
			}
			if e != nil || status != store.StatusOK {
				tx.Rollback(ctx)
				t.Fatal("first lock owner", status, e)
			}
			done := make(chan answer, 1)
			go func() {
				var a answer
				if reserveFirst {
					a.status, a.cells, a.err = ownedInputCall(db, legacy, args...)
				} else {
					a.status, a.cells, a.err = ownedInputCall(db, 63, sid, planUUID, "alice", "plan-owner", deliveryHash(text), digest, opaque)
				}
				done <- a
			}()
			waitPlanBlocked(t, p, tx.Conn().PgConn().PID())
			if e = tx.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			result := <-done
			want := store.StatusConflict
			if reserveFirst {
				want = store.StatusFailed
			}
			if result.err != nil || result.status != want {
				t.Fatal("second lock outcome", legacy, reserveFirst, result.status, result.err)
			}
			events := ownedInputCount(t, p, "session_ir_events", sid)
			plans := ownedInputCount(t, p, "session_input_plans", sid)
			if reserveFirst && (events != 0 || plans != 1) || !reserveFirst && (events != 2 || plans != 0) {
				t.Fatal("lock order partial rows", legacy, reserveFirst, events, plans)
			}
		}
	}
	// The real parent-row lock also orders65 and64, not a timing assumption.
	for _, closeFirst := range []bool{true, false} {
		sid := fmt.Sprintf("plan-terminal-lock-%t", closeFirst)
		if _, e := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'alice')`, sid); e != nil {
			t.Fatal(e)
		}
		r := reservePlan(t, db, sid, "alice", text, "zero")
		tx, e := p.Begin(ctx)
		if e != nil {
			t.Fatal(e)
		}
		q := ownedDeliveryTx{sessionIRLiveTx{tx}}
		beginArgs := []string{sid, planUUID, "planned-turn", "alice", batch, func() string {
			b, _ := json.Marshal(inputDeliveryIntent{1, r.Receiver, deliveryHash("submitted"), "signed-submitted"})
			return string(b)
		}(), r.ID, r.Hash}
		terminalArgs := []string{r.ID, sid, planUUID, "alice", r.Receiver, r.Hash, `{"fixture_verified_closed":true}`}
		var status uint32
		if closeFirst {
			status, _, e = sessionInputPlanTerminal(ctx, q, terminalArgs)
		} else {
			status, _, e = sessionInputPlannedBegin(ctx, q, beginArgs)
		}
		if e != nil || status != store.StatusOK {
			tx.Rollback(ctx)
			t.Fatal("first terminal/commit", status, e)
		}
		done := make(chan answer, 1)
		go func() {
			var a answer
			if closeFirst {
				a.status, a.cells, a.err = ownedInputCall(db, 64, beginArgs...)
			} else {
				a.status, a.cells, a.err = ownedInputCall(db, 65, terminalArgs...)
			}
			done <- a
		}()
		waitPlanBlocked(t, p, tx.Conn().PgConn().PID())
		if e = tx.Commit(ctx); e != nil {
			t.Fatal(e)
		}
		a := <-done
		if a.err != nil || a.status != store.StatusConflict {
			t.Fatal("second terminal/commit", closeFirst, a.status, a.err)
		}
		var state string
		if e = p.QueryRow(ctx, `SELECT state FROM session_input_plans WHERE id=$1::text::bigint`, r.ID).Scan(&state); e != nil {
			t.Fatal(e)
		}
		if closeFirst && (state != "terminal" || ownedInputCount(t, p, "session_ir_events", sid) != 0) || !closeFirst && (state != "committed" || ownedInputCount(t, p, "session_ir_events", sid) != 2) {
			t.Fatal("terminal/commit partial", state)
		}
	}
	t.Log("PASS actual pg_blocking_pids confirms parent-lock races: reserve↔56/58 both orders and65↔64 both orders; no partial visible events/outbox or plan reset")
}

func waitPlanBlocked(t *testing.T, p *pgxpool.Pool, blocker uint32) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		var blocked bool
		err := p.QueryRow(context.Background(), `SELECT EXISTS(SELECT 1 FROM pg_stat_activity a WHERE $1::integer=ANY(pg_blocking_pids(a.pid)))`, int64(blocker)).Scan(&blocked)
		if err != nil {
			t.Fatal(err)
		}
		if blocked {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatal("second operation never waited on actual parent-row lock")
}

func TestOwnedPlanEncodingBounds(t *testing.T) {
	db, p := deliveryFixture(t)
	if _, e := p.Exec(context.Background(), `INSERT INTO server_sessions(id,principal) VALUES('plan-encoding','alice')`); e != nil {
		t.Fatal(e)
	}
	opaque := strings.Repeat("\x01", 16384)
	digest := deliveryHash(opaque)
	c := ownedInputExpect(t, db, store.StatusOK, 63, "plan-encoding", planUUID, "alice", "plan-owner", deliveryHash("safe"), digest, opaque)
	var plan inputPlanRecord
	if len(c) != 1 || json.Unmarshal([]byte(c[0]), &plan) != nil || plan.Opaque != opaque || len(c[0]) < 6*len(opaque) {
		t.Fatal("escaped bound lost or unbounded")
	}
	ownedInputExpect(t, db, store.StatusInvalid, 63, "plan-encoding", planUUID, "alice", "plan-owner", deliveryHash("safe"), deliveryHash(opaque+"x"), opaque+"x")
	result := `{"x":"` + strings.Repeat(" ", 32760) + `"}`
	if len(result) != 32768 {
		t.Fatal("fixture bound")
	}
	ownedInputExpect(t, db, store.StatusInvalid, 65, plan.ID, plan.Session, plan.Operation, plan.Actor, plan.Receiver, plan.Hash, result+" ")
	ownedInputExpect(t, db, store.StatusOK, 65, plan.ID, plan.Session, plan.Operation, plan.Actor, plan.Receiver, plan.Hash, result)
	c = ownedInputExpect(t, db, store.StatusOK, 62, plan.Session, plan.Operation, plan.Actor)
	var out struct {
		Plan *inputPlanRecord `json:"plan"`
	}
	if len(c) != 1 || len(c[0]) >= 524288 || json.Unmarshal([]byte(c[0]), &out) != nil || out.Plan == nil || out.Plan.Opaque != opaque || out.Plan.TerminalResult != result {
		t.Fatal("complete escaped probe bound")
	}
	t.Log("PASS exact16KiB plan and32KiB terminal/complete escaped probe, +1 rejected without changing commitment")
}
