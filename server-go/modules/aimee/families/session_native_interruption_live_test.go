package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

// This fixture validates store custody and replay, not real kernel execution.
// Its descriptor is deliberately an explicitly named metadata surrogate.
func TestSessionNativeLiveInterruptionReceiptAndHistory(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	schema := fmt.Sprintf("native_interruption_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	config, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	config.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_session_harness.sql", "schema_learning_projection_outbox.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql"} {
		raw, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(raw)); err != nil {
			t.Fatal(err)
		}
	}
	run := func(op uint32, fields ...string) (uint32, []string, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		defer tx.Rollback(ctx)
		status, cells, err := Sessions.Ops[op].Run(ctx, sessionIRLiveTx{tx}, fields)
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, cells, err
	}
	encoded := func(value any) string { raw, _ := json.Marshal(value); return string(raw) }
	for _, mode := range []string{"restart-running", "restart-completed", "normal-failed"} {
		alreadyTerminal := mode != "restart-running"
		session := "interrupted-" + mode
		if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, session); err != nil {
			t.Fatal(err)
		}
		appendEvent := func(id, raw string) {
			status, _, err := run(28, session, id, "old", "actor", raw)
			if err != nil || status != store.StatusOK {
				t.Fatal(id, status, err)
			}
		}
		appendEvent("running", `{"kind":"execution","payload":{"status":"running"}}`)
		r := nativeReservationRequest{Version: 1, Session: session, Turn: "old", Harness: "codex", Binding: strings.Repeat("b", 64), Owner: strings.Repeat("a", 32), Descriptor: `{"version":1,"kind":"metadata-fixture-not-execution-proof"}`}
		sum := sha256.Sum256([]byte(r.Descriptor))
		r.Containment = hex.EncodeToString(sum[:])
		status, cells, err := run(opSessionNativeReserve, "actor", encoded(r))
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		r.Epoch = cells[0]
		claim := nativeRecoveryFixture()
		if alreadyTerminal {
			claim.Claim = strings.Repeat("9", 32)
		}
		if mode == "normal-failed" {
			claim.Claim = strings.Repeat("8", 32)
		}
		claim.Session = session
		claim.Turn = "old"
		claim.Harness = r.Harness
		claim.Binding = r.Binding
		claim.Owner = r.Owner
		claim.Containment = r.Containment
		claim.Epoch = r.Epoch
		if status, _, err := run(opSessionNativeOwnerClaim, "actor", encoded(claim)); err != nil || status != store.StatusOK {
			t.Fatal("claim", status, err)
		}
		owner := canonical.NativeExecutionOwner{Version: 1, Harness: r.Harness, ToolInterruptionOwner: canonical.ToolInterruptionOwner{Owner: r.Owner, Binding: r.Binding, Epoch: r.Epoch, Containment: r.Containment}}
		appendOwned := func(id, raw string) {
			var event map[string]json.RawMessage
			if json.Unmarshal([]byte(raw), &event) != nil {
				t.Fatal("bad fixture")
			}
			event["native_owner"] = json.RawMessage(encoded(owner))
			protected := encoded(event)
			for _, op := range []uint32{28, 36} {
				if status, _, err := run(op, session, id, "old", "actor", protected); err != nil || status != store.StatusInvalid {
					t.Fatal("ordinary owner forgery", status, err)
				}
			}
			if id == "original" {
				if status, _, err := run(opSessionNativeSourceAppend, session, "foreign-source", "old", "foreign", protected); err != nil || status == store.StatusOK {
					t.Fatal("foreign source", status, err)
				}
				changed := owner
				changed.Epoch = strings.Repeat("f", 32)
				event["native_owner"] = json.RawMessage(encoded(changed))
				if status, _, err := run(opSessionNativeSourceAppend, session, "stale-epoch", "old", "actor", encoded(event)); err != nil || status == store.StatusOK {
					t.Fatal("stale source epoch", status, err)
				}
			}
			status, _, err := run(opSessionNativeSourceAppend, session, id, "old", "actor", protected)
			if err != nil || status != store.StatusOK {
				t.Fatal("protected source", id, status, err)
			}
		}
		// More than nine observations catches accidental lexicographic sequence order.
		for i := 0; i < 5; i++ {
			appendOwned(fmt.Sprintf("paired-call-%d", i), fmt.Sprintf(`{"kind":"message","message_id":"paired-call-%d","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"reused-id","name":"read_file","input":{}}]}}`, i))
			appendOwned(fmt.Sprintf("paired-result-%d", i), fmt.Sprintf(`{"kind":"message","message_id":"paired-result-%d","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"reused-id","content":"actual result"}]}}`, i))
		}
		original := `{"kind":"message","message_id":"original-assistant","payload":{"role":"assistant","blocks":[{"type":"text","text":"original context"},{"type":"tool_use","tool_id":"uncertain","name":"bash","input":{"value":123.123456789012345678901234567890}},{"type":"tool_use","tool_id":"observed","name":"read_file","input":{}}]}}`
		var prefix map[string]json.RawMessage
		json.Unmarshal([]byte(original), &prefix)
		var message struct {
			Role   string            `json:"role"`
			Blocks []json.RawMessage `json:"blocks"`
		}
		json.Unmarshal(prefix["payload"], &message)
		message.Blocks = message.Blocks[:2]
		prefix["payload"] = json.RawMessage(encoded(message))
		prefix["partial"] = json.RawMessage("true")
		appendOwned("original", encoded(prefix))
		appendOwned("original-final", original)
		prefix["native_owner"] = json.RawMessage(encoded(owner))
		if status, _, err := run(opSessionNativeSourceAppend, session, "stale-short-prefix", "old", "actor", encoded(prefix)); err != nil || status == store.StatusOK {
			t.Fatal("protected prefix shrank", status, err)
		}
		appendOwned("actual-result", `{"kind":"message","message_id":"actual-result","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"observed","content":"actual value"}]}}`)
		if alreadyTerminal {
			terminal := "completed"
			if mode == "normal-failed" {
				terminal = "failed"
			}
			appendEvent("actual-terminal", fmt.Sprintf(`{"kind":"execution","payload":{"status":%q}}`, terminal))
		}
		r.Descriptor = ""
		r.Expected = claim.token("pending")
		op := opSessionNativeReservationReconcile
		expectedCells := 2
		if mode == "normal-failed" {
			op = opSessionNativeReservationFinish
			expectedCells = 3
			r.Expected = claim.token("invalid")
			if _, err := pool.Exec(ctx, `UPDATE session_harness_handles SET native_id=$3 WHERE session_id=$1 AND harness=$2`, claim.Session, claim.key(), r.Expected); err != nil {
				t.Fatal(err)
			}
		}
		status, recovered, err := run(op, "actor", encoded(r))
		if err != nil || status != store.StatusOK || len(recovered) != expectedCells {
			t.Fatal(status, recovered, err)
		}
		status, retry, err := run(op, "actor", encoded(r))
		if err != nil || status != store.StatusOK || fmt.Sprint(retry) != fmt.Sprint(recovered) {
			t.Fatal("recovery replay duplicated", status, retry, err)
		}
		var count int
		var receiptRaw string
		if err := pool.QueryRow(ctx, `SELECT count(*),max(payload) FROM session_ir_events WHERE session_id=$1 AND payload::jsonb->>'kind'='tool_interruption'`, session).Scan(&count, &receiptRaw); err != nil || count != 1 {
			t.Fatal(count, err)
		}
		var receipt struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ToolInterruption `json:"payload"`
		}
		if !canonical.Decode([]byte(receiptRaw), &receipt) || receipt.Payload.Source.EventID != "original" || receipt.Payload.Source.BlockIndex != 1 || receipt.Payload.Source.ToolID != "uncertain" || receipt.Payload.Terminal != recovered[0] {
			t.Fatal(receiptRaw)
		}
		var terminal string
		if err := pool.QueryRow(ctx, `SELECT payload::jsonb->'payload'->>'status' FROM session_ir_events WHERE session_id=$1 AND sequence=$2::bigint`, session, recovered[0]).Scan(&terminal); err != nil || (mode == "restart-completed" && terminal != "completed") || (mode == "normal-failed" && terminal != "failed") || (!alreadyTerminal && terminal != "cancelled") {
			t.Fatal(terminal, err)
		}
		status, history, err := run(opSessionNativeHistory, "actor", session, "completed", "")
		if err != nil || status != store.StatusOK || len(history) != 1 {
			t.Fatal(status, err)
		}
		if !strings.Contains(history[0], "123.123456789012345678901234567890") || !strings.Contains(history[0], canonical.ToolInterruptionDomain) || !strings.Contains(history[0], "actual value") || !strings.Contains(history[0], "explicit_permission_required") {
			t.Fatal(history)
		}
		// Isolated archive views must not resolve primary unknown-call receipts.
		archival := `[{"role":"user","blocks":[{"type":"text","text":"isolated external archive"}]}]`
		if status, _, err := run(opSessionIRHistorySave, session, "external-checkpoint", "external-sdk:claude-code", "actor", archival); err != nil || status != store.StatusOK {
			t.Fatal("external archive checkpoint", status, err)
		}
		if status, cells, err := run(opSessionIRHistoryLoad, session, "external-sdk:claude-code", "actor", ""); err != nil || status != store.StatusOK || len(cells) != 1 || !strings.Contains(cells[0], "isolated external archive") {
			t.Fatal("primary receipt poisoned isolated archive", status, cells, err)
		}
		var projection struct {
			Messages json.RawMessage `json:"messages"`
		}
		if json.Unmarshal([]byte(history[0]), &projection) != nil {
			t.Fatal("projection")
		}
		if status, _, err := run(opSessionIRHistorySave, session, "checkpoint", "primary", "actor", string(projection.Messages)); err != nil || status != store.StatusOK {
			t.Fatal("checkpoint", status, err)
		}
		status, checkpointed, err := run(opSessionNativeHistory, "actor", session, "completed", "")
		if err != nil || status != store.StatusOK || len(checkpointed) != 1 || checkpointed[0] != history[0] {
			t.Fatal("checkpoint lost authenticated receipt", status, err, checkpointed)
		}
		// A fresh transaction sees the same durable projection and sidecar.
		_, historyAgain, err := run(opSessionNativeHistory, "actor", session, "completed", "")
		if err != nil || fmt.Sprint(historyAgain) != fmt.Sprint(history) {
			t.Fatal("restart projection changed", err)
		}
		// Compaction may omit old observations, but cannot erase the durable
		// unknown-effect permission fence or create a source from its summary.
		omitted := `[{"role":"user","blocks":[{"type":"text","text":"host compacted current history"}]}]`
		if status, _, err := run(opSessionIRHistorySave, session, "compact-checkpoint", "primary", "actor", omitted); err != nil || status != store.StatusOK {
			t.Fatal("compact checkpoint", status, err)
		}
		status, compacted, err := run(opSessionNativeHistory, "actor", session, "completed", "")
		var compactView struct {
			Messages      []canonical.Message `json:"messages"`
			Interruptions []json.RawMessage   `json:"interruptions"`
		}
		if err != nil || status != store.StatusOK || len(compacted) != 1 || json.Unmarshal([]byte(compacted[0]), &compactView) != nil || len(compactView.Messages) != 1 || len(compactView.Interruptions) != 1 {
			t.Fatal("compaction erased source permission fence", status, err, compacted)
		}
		for _, op := range []uint32{28, 36} {
			status, _, err := run(op, session, "forged-receipt", "old", "actor", receiptRaw)
			if err != nil || status != store.StatusInvalid {
				t.Fatal("forged receipt admitted", op, status, err)
			}
		}
		status, _, err = run(opSessionNativeHistory, "foreign", session, "completed", "")
		if err != nil || status != store.StatusInvalid {
			t.Fatal("foreign sidecar", status, err)
		}
		if _, err := pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id=$1 AND event_id='original'`, session); err != nil {
			t.Fatal(err)
		}
		if status, _, err = run(opSessionNativeHistory, "actor", session, "completed", ""); err == nil {
			t.Fatal("erased exact call retained admission authority", status)
		}
	}
}
