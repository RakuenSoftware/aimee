package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	ep "github.com/JBailes/aimee/server-go/modules/execution-policy"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestOperatorCancelHostOnlyAndReservedNamespace(t *testing.T) {
	frame, _ := wire.EncodeFields(opSessionIRCancel, []string{"owner", "s", "turn"})
	db := newSessDB()
	_, status := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
	if !Sessions.Ops[opSessionIRCancel].HostOnly || status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("model token acquired operator cancellation", status)
	}
	for _, payload := range []string{operatorCancelPayload, `{"kind":"message","message_id":"fake","payload":{"role":"assistant","blocks":[]}}`} {
		status, _, err := sessionIRAppend(context.Background(), db, []string{"s", "operator-cancel:turn", "turn", "owner", payload})
		if err != nil || status != store.StatusInvalid {
			t.Fatal("reserved operator namespace accepted", status, err)
		}
	}
}

func TestSessionIRLiveExplicitOperatorCancelFence(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("operator_cancel_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	cfg, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql", "schema_learning_projection_outbox.sql", "schema_governed_actions.sql"} {
		raw, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(raw)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('s','owner'),('legacy','owner'),('child','owner'),('foreign-child','foreign')`); err != nil {
		t.Fatal(err)
	}
	// Prior releases accepted arbitrary event IDs. A prefix collision, another
	// turn, another actor, or another current owner is never a privileged fence.
	for i, source := range []struct{ owner, actor, id, turn, payload string }{
		{"historic-owner", "historic-owner", "operator-cancel:old", "old", `{"kind":"message","message_id":"old","payload":{"role":"assistant","blocks":[]}}`},
		{"historic-owner", "historic-owner", "operator-cancel:other", "old", operatorCancelPayload},
		{"historic-owner", "foreign", "operator-cancel:old", "old", operatorCancelPayload},
		{"foreign", "historic-owner", "operator-cancel:old", "old", operatorCancelPayload},
	} {
		session := fmt.Sprint("historic-", i)
		if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,$2)`, session, source.owner); err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,1,$2,$3,$4,$5)`, session, source.id, source.turn, source.actor, source.payload); err != nil {
			t.Fatal(err)
		}
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		fenced, err := nativeRecoveryFenced(ctx, sessionIRLiveTx{tx}, session, "old")
		_ = tx.Rollback(ctx)
		if err != nil || fenced {
			t.Fatal("historic marker acquired continuation fence", i, fenced, err)
		}
	}
	txHistoric, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	allowed, err := sessionActionTurnAdmitted(ctx, sessionIRLiveTx{txHistoric}, "historic-0", "historic-owner", "")
	_ = txHistoric.Rollback(ctx)
	if err != nil || !allowed {
		t.Fatal("historic marker acquired actor-wide fence", allowed, err)
	}

	run := func(fn store.OpFunc, fields ...string) (uint32, []string) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := fn(ctx, sessionIRLiveTx{tx}, fields)
		if err != nil {
			t.Fatal(err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
		return status, cells
	}
	_ = run
	appendEvent := func(id, turn, payload string) {
		t.Helper()
		status, _ := run(sessionIRAppend, "s", id, turn, "owner", payload)
		if status != store.StatusOK {
			t.Fatal("append", status)
		}
	}
	appendEvent("running", "turn", `{"kind":"execution","payload":{"status":"running"}}`)
	appendEvent("user", "turn", `{"kind":"message","message_id":"user","payload":{"role":"user","blocks":[{"type":"text","text":"original request"}]}}`)

	appendEvent("prior-call", "turn", `{"kind":"message","message_id":"prior-assistant","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"question-call","name":"read_file","input":{"path":"README"}}]}}`)
	appendEvent("prior-result", "turn", `{"kind":"message","message_id":"prior-tool","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"question-call","content":"actual owner receipt","is_error":false}]}}`)
	for i := 0; i < 9; i++ {
		appendEvent(fmt.Sprint("observed-", i), "turn", fmt.Sprintf(`{"kind":"message","message_id":"observed-%d","payload":{"role":"assistant","blocks":[{"type":"text","text":"observed progress"}]}}`, i))
	}
	appendEvent("api-call", "turn", `{"kind":"message","message_id":"api-assistant","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"question-call","name":"request_input","input":{"question":"Pick one","exact":0.123456789012345678901234567890}}]}}`)
	appendEvent("api-question", "turn", `{"kind":"control_request","request_id":"question","control_kind":"question","payload":{"question":"Pick one"}}`)
	for _, fields := range [][]string{{"foreign", "s", "turn"}, {"owner", "s", "invented"}} {
		if status, _ := run(sessionIRCancel, fields...); status == store.StatusOK {
			t.Fatal("foreign or invented cancel accepted", fields)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO session_native_reservations(session_id,harness,binding_sha256,owner_id,turn_id,actor,epoch,containment_sha256,containment_json) VALUES('s','codex','binding','native-owner','turn','owner','0','containment','{}')`); err != nil {
		t.Fatal(err)
	}
	if status, _ := run(sessionIRCancel, "owner", "s", "turn"); status != store.StatusInvalid {
		t.Fatal("active native custody bypassed", status)
	}
	if _, err = pool.Exec(ctx, `UPDATE session_native_reservations SET state='finished',terminal_sequence=1`); err != nil {
		t.Fatal(err)
	}
	if _, err = pool.Exec(ctx, `INSERT INTO session_native_cache_owners(session_id,cache_key,actor,turn_id,harness,scope_sha256,context_sha256,binding_sha256,owner_id,claim_id,containment_sha256,cache_epoch) VALUES('s','prior-cache','owner','prior-native','codex','scope','context','binding','owner','claim','containment','0')`); err != nil {
		t.Fatal(err)
	}
	if status, _ := run(sessionIRCancel, "owner", "s", "turn"); status != store.StatusInvalid {
		t.Fatal("unresolved native cache custody bypassed", status)
	}
	if _, err = pool.Exec(ctx, `UPDATE session_native_reservations SET owner_id='owner',turn_id='prior-native',claim_id='claim'`); err != nil {
		t.Fatal(err)
	}
	status, cells := run(sessionIRCancel, "owner", "s", "turn")
	if status != store.StatusOK || len(cells) != 1 {
		t.Fatal("cancel", status, cells)
	}
	first := cells[0]
	status, cells = run(sessionIRCancel, "owner", "s", "turn")
	if status != store.StatusOK || len(cells) != 1 || cells[0] != first {
		t.Fatal("cancel retry not idempotent", status, cells)
	}
	// Immutable retries remain acknowledgments; all new writes from the fenced turn fail.
	if status, _ = run(sessionIRAppend, "s", "running", "turn", "owner", `{"kind":"execution","payload":{"status":"running"}}`); status != store.StatusOK {
		t.Fatal("immutable retry lost", status)
	}
	for _, payload := range []string{`{"kind":"execution","payload":{"status":"completed"}}`, `{"kind":"message","message_id":"late","payload":{"role":"assistant","blocks":[]}}`, `{"kind":"message","message_id":"late-result","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"question-call","content":"invented late answer","is_error":false}]}}`} {
		if status, _ = run(sessionIRAppend, "s", "late", "turn", "owner", payload); status != store.StatusInvalid {
			t.Fatal("late worker escaped fence", status)
		}
	}
	// Existing history refusal stays intact until this explicit terminal; original prompt survives.
	if status, cells = run(sessionIRHistoryLoad, "s", "primary", "owner", "fresh"); status != store.StatusOK || len(cells) != 1 || !strings.Contains(cells[0], "original request") {
		t.Fatal("history not recoverable", status, cells)
	}

	projected := cells[0]
	if !strings.Contains(projected, "0.123456789012345678901234567890") {
		t.Fatal("API call numeric precision changed")
	}
	if !strings.Contains(projected, canonical.ToolInterruptionText) || !strings.Contains(projected, `"name":"request_input"`) {
		t.Fatal("cancelled API call lost honest unknown result", projected)
	}
	status, sidecar := run(sessionNativeHistory, "owner", "s", "completed", "")
	var qualified struct {
		Messages      json.RawMessage `json:"messages"`
		Interruptions []struct {
			ID      string                     `json:"event_id"`
			Receipt canonical.ToolInterruption `json:"receipt"`
			Call    canonical.Block            `json:"call"`
		} `json:"interruptions"`
	}
	if status != store.StatusOK || len(sidecar) != 1 || json.Unmarshal([]byte(sidecar[0]), &qualified) != nil || len(qualified.Interruptions) != 1 || qualified.Interruptions[0].Receipt.Owner.OperatorCancel != "operator-cancel:turn" || qualified.Interruptions[0].Call.Name != "request_input" {
		t.Fatal("operator API sidecar", status, sidecar)
	}
	if _, err := canonical.DecodeMessages(qualified.Messages); err != nil {
		t.Fatal("typed operator history", err)
	}
	var projectedMessages []canonical.Message
	_ = json.Unmarshal(qualified.Messages, &projectedMessages)
	pending := 0
	results := 0
	unknown := 0
	for _, m := range projectedMessages {
		for _, b := range m.Blocks {
			if b.ToolID == "question-call" {
				if b.Type == "tool_use" {
					pending++
				}
				if b.Type == "tool_result" {
					pending--
					results++
					if b.Context != nil && b.Context.RevisionDomain == canonical.ToolInterruptionDomain {
						unknown++
						if b.IsError == nil || !*b.IsError {
							t.Fatal("unknown result claimed success")
						}
					}
				}
			}
		}
	}
	if pending != 0 || results != 2 || unknown != 1 {
		t.Fatal("interrupted ordered cycle remains open", pending, results)
	}
	var countBefore, countAfter int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='s'`).Scan(&countBefore); err != nil {
		t.Fatal(err)
	}
	run(sessionIRCancel, "owner", "s", "turn")
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='s'`).Scan(&countAfter); err != nil || countAfter != countBefore {
		t.Fatal("cancel duplicated receipts", countBefore, countAfter, err)
	}
	if status, _ = run(sessionIRAppend, "s", "operator-interruption:turn:3:0", "turn", "owner", operatorCancelPayload); status != store.StatusInvalid {
		t.Fatal("operator receipt namespace forged")
	}
	if status, _ = run(sessionIRHistorySave, "s", "api-checkpoint", "primary", "owner", string(qualified.Messages)); status != store.StatusOK {
		t.Fatal("operator checkpoint", status)
	}
	if status, sidecar = run(sessionNativeHistory, "owner", "s", "completed", ""); status != store.StatusOK || len(sidecar) != 1 {
		t.Fatal("operator checkpoint lost authority", status, sidecar)
	}
	for _, mutation := range []string{
		`UPDATE session_ir_events SET payload=replace(payload,'request_input','forged_tool') WHERE session_id='s' AND event_id='api-call'`,
		`UPDATE session_ir_events SET payload=replace(payload,'Operator cancelled continuation','Forged continuation') WHERE session_id='s' AND event_id='operator-cancel:turn'`,
		`UPDATE session_ir_events SET actor='foreign' WHERE session_id='s' AND event_id='api-call'`,
		`UPDATE session_ir_events SET payload=replace(payload,'Pick one','changed question') WHERE session_id='s' AND event_id='api-call'`,
	} {
		trial, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = trial.Exec(ctx, mutation); err != nil {
			t.Fatal(err)
		}
		status, _, err := sessionNativeHistory(ctx, sessionIRLiveTx{trial}, []string{"owner", "s", "completed", ""})
		_ = trial.Rollback(ctx)
		if err == nil && status == store.StatusOK {
			t.Fatal("changed source/marker retained operator authority")
		}
	}

	// Compaction may omit the entire old tool cycle while retaining its durable
	// permission fence. Retaining only its call without the diagnostic refuses.
	var compacted []json.RawMessage
	if json.Unmarshal(qualified.Messages, &compacted) != nil {
		t.Fatal("compaction input")
	}
	trial, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	withoutResult, _ := json.Marshal(compacted[:len(compacted)-1])
	if status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{trial}, []string{"s", "broken-checkpoint", "primary", "owner", string(withoutResult)}); err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	status, _, err = sessionNativeHistory(ctx, sessionIRLiveTx{trial}, []string{"owner", "s", "completed", ""})
	_ = trial.Rollback(ctx)
	if err == nil && status == store.StatusOK {
		t.Fatal("checkpoint dropped unknown diagnostic while retaining its call")
	}
	if status, _ = run(sessionIRHistorySave, "s", "compacted-checkpoint", "primary", "owner", `[{"role":"user","blocks":[{"type":"text","text":"compacted prior conversation"}]}]`); status != store.StatusOK {
		t.Fatal("compacted checkpoint", status)
	}
	if status, sidecar = run(sessionNativeHistory, "owner", "s", "completed", ""); status != store.StatusOK || len(sidecar) != 1 || !strings.Contains(sidecar[0], `"operator_cancel_event_id":"operator-cancel:turn"`) {
		t.Fatal("compaction erased operator permission fence", status, sidecar)
	}
	appendEvent("next-start", "next", `{"kind":"execution","payload":{"status":"running"}}`)
	action := func(turn, operation string, intent ep.ActionIntent) uint32 {
		t.Helper()
		raw, _ := json.Marshal(governedActionRequest{TurnSession: func() string {
			if turn != "" {
				return "s"
			}
			return ""
		}(), Turn: turn, Operation: operation, Intent: intent})
		status, _ := run(governedActionApply, "owner", "s", string(raw))
		return status
	}
	for _, turn := range []string{"turn", "invented"} {
		for _, op := range []string{"issue", "admit", "dispatch"} {
			if action(turn, op, ep.ActionIntent{TurnSession: "s", Turn: turn}) != store.StatusInvalid {
				t.Fatal("fenced/unbound/invented action admitted", turn, op)
			}
		}
	}
	for _, op := range []string{"issue", "admit", "dispatch"} {
		raw, _ := json.Marshal(governedActionRequest{Operation: op})
		status, cells := run(governedActionApply, "owner", "legacy", string(raw))
		if status != store.StatusOK || len(cells) != 1 || !strings.Contains(cells[0], `"reason":"canonical_turn_binding_required"`) {
			t.Fatal("same actor bypassed fence through other unbound project", status, cells)
		}
	}
	if action("next", "admit", ep.ActionIntent{TurnSession: "s", Turn: "turn"}) != store.StatusInvalid {
		t.Fatal("stored intent rebound to different turn")
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	for _, test := range []struct {
		sid, turn string
		want      bool
	}{{"s", "next", true}, {"s", "turn", false}, {"s", "", false}, {"legacy", "", false}} {
		got, err := sessionActionTurnAdmitted(ctx, sessionIRLiveTx{tx}, test.sid, "owner", test.turn)
		if err != nil || got != test.want {
			t.Fatal("turn admission", test, got, err)
		}
	}
	if allowed, err := sessionActionTurnAdmitted(ctx, sessionIRLiveTx{tx}, "foreign-project", "foreign", ""); err != nil || !allowed {
		t.Fatal("foreign actor legacy calls affected", allowed, err)
	}
	_ = tx.Rollback(ctx)
	if status, _ = run(sessionIRAppend, "child", "child-start", "child-turn", "owner", `{"kind":"execution","payload":{"status":"running"}}`); status != store.StatusOK {
		t.Fatal("child start", status)
	}
	t.Setenv("AIMEE_HOME", t.TempDir())
	arguments := json.RawMessage(`{"path":"/workspace/result"}`)
	request := governedActionRequest{TurnSession: "child", Turn: "child-turn", Operation: "issue", RegistryClass: "file_write", Arguments: arguments,
		Intent: ep.ActionIntent{Request: "request", Task: "task", Attempt: "attempt", Tool: "write_file", Class: "file_write", Destination: "file:/workspace/result", PayloadDigest: actionTestHash(arguments), Purpose: "requested edit", RevocationGeneration: "revocation:1", WorkUnits: fmt.Sprint(len(arguments))}}
	callAction := func() ep.ActionDecision {
		t.Helper()
		raw, _ := json.Marshal(request)
		status, cells := run(governedActionApply, "owner", "s", string(raw))
		var decision ep.ActionDecision
		if status != store.StatusOK || len(cells) != 1 || json.Unmarshal([]byte(cells[0]), &decision) != nil {
			t.Fatal("action transition", request.Operation, status, cells)
		}
		return decision
	}
	issued := callAction()
	if !issued.Allowed || issued.Receipt == nil || issued.Receipt.Intent.Turn != "child-turn" || issued.Receipt.Intent.TurnSession != "child" {
		t.Fatal("host turn not stored in issued intent", issued)
	}
	request.Intent = issued.Receipt.Intent
	now := time.Now().UTC()
	request.Freshness = ep.ActionFreshness{PolicyGeneration: request.Intent.PolicyGeneration, RevocationGeneration: request.Intent.RevocationGeneration, Checked: now, Expires: now.Add(2 * time.Second), Authorized: true}
	request.Operation = "admit"
	if d := callAction(); !d.Allowed {
		t.Fatal("bound admission", d)
	}
	request.Operation = "dispatch"
	if d := callAction(); !d.DispatchAllowed {
		t.Fatal("bound dispatch", d)
	}
	if status, _ = run(sessionIRCancel, "owner", "child", "child-turn"); status != store.StatusOK {
		t.Fatal("explicit stop with dispatched effect", status)
	}
	request.Operation = "cancel"
	if d := callAction(); d.Allowed || d.Reason != "cancellation_requires_reconciliation" || d.Receipt == nil || d.Receipt.State != "dispatching" {
		t.Fatal("operator stop relabelled dispatched effect", d)
	}
	request.Operation = "outcome"
	request.Outcome = &ep.ActionOutcome{State: "outcome_unknown", EvidenceRef: "lost-connection", Destination: request.Intent.Destination, PayloadDigest: request.Intent.PayloadDigest}
	if d := callAction(); !d.Allowed || d.Receipt == nil || d.Receipt.State != "outcome_unknown" {
		t.Fatal("unknown effect receipt lost after fence", d)
	}
	request.Operation = "reconcile"
	request.Outcome = &ep.ActionOutcome{State: "effect_confirmed", EvidenceRef: "independent-object-observation", ObjectVersion: "sha256:confirmed", Destination: request.Intent.Destination, PayloadDigest: request.Intent.PayloadDigest}
	if d := callAction(); !d.Allowed || d.Receipt == nil || d.Receipt.State != "effect_confirmed" {
		t.Fatal("effect reconciliation blocked after fence", d)
	}

	// Actor-wide legacy admission and operator cancellation serialize even when
	// the action journal and canonical destination are different session rows.
	for _, cancelFirst := range []bool{false, true} {
		actor := fmt.Sprint("race-owner-", cancelFirst)
		journal := fmt.Sprint("race-journal-", cancelFirst)
		destination := fmt.Sprint("race-canonical-", cancelFirst)
		if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,$3),($2,$3)`, journal, destination, actor); err != nil {
			t.Fatal(err)
		}
		if status, _ = run(sessionIRAppend, destination, "start", "race-turn", actor, `{"kind":"execution","payload":{"status":"running"}}`); status != store.StatusOK {
			t.Fatal("race start", status)
		}
		legacy := governedActionRequest{Operation: "issue", RegistryClass: "file_write", Arguments: arguments, Intent: ep.ActionIntent{Request: "race-request", Task: "task", Attempt: "attempt", Tool: "write_file", Class: "file_write", Destination: "file:/workspace/result", PayloadDigest: actionTestHash(arguments), Purpose: "requested edit", RevocationGeneration: "revocation:1", WorkUnits: fmt.Sprint(len(arguments))}}
		body, _ := json.Marshal(legacy)
		status, cells = run(governedActionApply, actor, journal, string(body))
		var issued ep.ActionDecision
		if status != store.StatusOK || len(cells) != 1 || json.Unmarshal([]byte(cells[0]), &issued) != nil || issued.Receipt == nil {
			t.Fatal("race issue", status, cells)
		}
		legacy.Intent = issued.Receipt.Intent
		legacy.Operation = "admit"
		now := time.Now().UTC()
		legacy.Freshness = ep.ActionFreshness{PolicyGeneration: legacy.Intent.PolicyGeneration, RevocationGeneration: legacy.Intent.RevocationGeneration, Checked: now, Expires: now.Add(2 * time.Second), Authorized: true}
		body, _ = json.Marshal(legacy)
		status, cells = run(governedActionApply, actor, journal, string(body))
		if status != store.StatusOK {
			t.Fatal("race admit", status, cells)
		}
		legacy.Operation = "dispatch"
		body, _ = json.Marshal(legacy)
		held, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var firstStatus uint32
		if cancelFirst {
			firstStatus, _, err = sessionIRCancel(ctx, sessionIRLiveTx{held}, []string{actor, destination, "race-turn"})
		} else {
			firstStatus, _, err = governedActionApply(ctx, sessionIRLiveTx{held}, []string{actor, journal, string(body)})
		}
		if err != nil || firstStatus != store.StatusOK {
			_ = held.Rollback(ctx)
			t.Fatal("race first transaction", firstStatus, err)
		}
		type raceResult struct {
			status uint32
			cells  []string
			err    error
		}
		done := make(chan raceResult, 1)
		ready := make(chan struct{})
		go func() {
			waiting, err := pool.Begin(ctx)
			if err != nil {
				done <- raceResult{err: err}
				return
			}
			defer waiting.Rollback(ctx)
			close(ready)
			var status uint32
			var cells []string
			if cancelFirst {
				status, cells, err = governedActionApply(ctx, sessionIRLiveTx{waiting}, []string{actor, journal, string(body)})
			} else {
				status, cells, err = sessionIRCancel(ctx, sessionIRLiveTx{waiting}, []string{actor, destination, "race-turn"})
			}
			if err == nil {
				err = waiting.Commit(ctx)
			}
			done <- raceResult{status, cells, err}
		}()
		<-ready
		select {
		case result := <-done:
			_ = held.Rollback(ctx)
			t.Fatal("cross-project admission did not wait for cancellation authority", result)
		case <-time.After(50 * time.Millisecond):
		}
		if err = held.Commit(ctx); err != nil {
			t.Fatal(err)
		}
		result := <-done
		if result.err != nil || result.status != store.StatusOK {
			t.Fatal("race second transaction", result)
		}
		if cancelFirst && (len(result.cells) != 1 || !strings.Contains(result.cells[0], `"reason":"canonical_turn_binding_required"`)) {
			t.Fatal("late unbound dispatch escaped committed cancellation", result)
		}
		if !cancelFirst && len(result.cells) != 1 {
			t.Fatal("already dispatched effect prevented explicit continuation cancel", result)
		}
	}

	for _, fault := range []string{"foreign-actor", "native-owner"} {
		sid := "refuse-" + fault
		if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'owner')`, sid); err != nil {
			t.Fatal(err)
		}
		if status, _ = run(sessionIRAppend, sid, "start", "unowned-turn", "owner", `{"kind":"execution","payload":{"status":"running"}}`); status != store.StatusOK {
			t.Fatal(status)
		}
		payload := `{"kind":"message","message_id":"unowned-call","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"effect","name":"write_file","input":{}}]}}`
		actor := "foreign"
		if fault == "native-owner" {
			actor = "owner"
			var event map[string]any
			_ = json.Unmarshal([]byte(payload), &event)
			event["native_owner"] = canonical.NativeExecutionOwner{Version: 1, Harness: "codex", ToolInterruptionOwner: canonical.ToolInterruptionOwner{Owner: strings.Repeat("a", 32), Binding: strings.Repeat("b", 64), Epoch: "0", Containment: strings.Repeat("c", 64)}}
			raw, _ := json.Marshal(event)
			payload = string(raw)
		}
		if _, err = pool.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,2,'unowned','unowned-turn',$2,$3)`, sid, actor, payload); err != nil {
			t.Fatal(err)
		}
		trial, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		status, _, err := sessionIRCancel(ctx, sessionIRLiveTx{trial}, []string{"owner", sid, "unowned-turn"})
		_ = trial.Rollback(ctx)
		if err == nil && status == store.StatusOK {
			t.Fatal("operator cancellation adopted foreign/native source", fault)
		}
		var terminals int
		if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id=$1 AND event_id LIKE 'operator-cancel:%'`, sid).Scan(&terminals); err != nil || terminals != 0 {
			t.Fatal("refused source partially committed cancellation", terminals, err)
		}
	}

}
