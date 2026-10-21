package families

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

func runningHistoryRows(events ...[2]string) [][]any {
	rows := make([][]any, len(events))
	for i, event := range events {
		rows[i] = []any{fmt.Sprint(i + 1), event[0], "owner", fmt.Sprintf("event-%d", i+1), event[1]}
	}
	return rows
}

const runningStart = `{"kind":"execution","payload":{"status":"running"}}`
const runningDone = `{"kind":"execution","payload":{"status":"completed"}}`
const runningUser = `{"kind":"message","message_id":"user","payload":{"role":"user","blocks":[{"type":"text","text":"current user"}]}}`

func TestSessionIRRunningHistoryIncludesOnlyOwnedCommittedConversation(t *testing.T) {
	db := newSessDB()
	original := `{"kind":"message","message_id":"wire-id","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"call:original","name":"exec_command","input":{"value":123.123456789012345678901234567890}}]}}`
	db.rows = runningHistoryRows(
		[2]string{"old", runningStart}, [2]string{"old", runningUser}, [2]string{"old", runningDone},
		[2]string{"active", runningStart}, [2]string{"active", runningUser}, [2]string{"active", original},
		[2]string{"active", original}, // snapshots replace by message identity, not duplicate history
	)
	status, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal(status, cells, err)
	}
	var history []json.RawMessage
	if err = json.Unmarshal([]byte(cells[0]), &history); err != nil || len(history) != 3 {
		t.Fatal(cells, err)
	}
	for _, exact := range []string{"call:original", "exec_command", "123.123456789012345678901234567890"} {
		if !strings.Contains(cells[0], exact) {
			t.Fatal("source content changed", exact, cells)
		}
	}
	if db.executed[0] != sessionIRRunningHistorySQL || fmt.Sprint(db.args[0]) != "[s owner 0]" {
		t.Fatal("owner query changed", db.executed, db.args)
	}
	// Completed-history callers retain their original contract: the open turn is
	// permitted by the active ID but excluded from the returned conversation.
	legacy := newSessDB()
	for _, row := range db.rows {
		legacy.rows = append(legacy.rows, row)
	}
	status, cells, err = sessionIRHistoryLoad(context.Background(), legacy, []string{"s", "primary", "owner", "active"})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal(status, cells, err)
	}
	if err = json.Unmarshal([]byte(cells[0]), &history); err != nil || len(history) != 1 {
		t.Fatal("legacy pending turn leaked", cells, err)
	}
}

// Provider TURN_START precedes SDK execution, but its final message snapshot
// can arrive after an actual tool result. Both running and completed consumers
// must retain causal order without making unmatched deltas into content.
func TestSessionIRHistoryOrdersStreamSnapshotBeforeEarlyToolResult(t *testing.T) {
	const start = `{"kind":"delta","message_id":"provider-response","role":"assistant","payload":{"type":"TURN_START"}}`
	const original = `{"kind":"message","message_id":"provider-response","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"original-call","namespace":"aimee","name":"read_file","input":{"value":0.123456789012345678901234567890}}]}}`
	const result = `{"kind":"message","message_id":"actual-tool-result","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"original-call","content":"actual result","is_error":false}]}}`
	const unmatched = `{"kind":"delta","message_id":"no-snapshot","role":"assistant","payload":{"type":"TURN_START"}}`
	for _, running := range []bool{true, false} {
		for _, repeat := range []bool{false, true} {
			db := newSessDB()
			events := [][2]string{{"active", runningStart}, {"active", runningUser}, {"active", unmatched}, {"active", start}, {"active", result}, {"active", original}}
			if repeat {
				events = append(events, [2]string{"active", original})
			}
			if !running {
				events = append(events, [2]string{"active", runningDone})
			}
			db.rows = runningHistoryRows(events...)
			var status uint32
			var cells []string
			var err error
			if running {
				status, cells, err = sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
			} else {
				status, cells, err = sessionIRHistoryLoad(context.Background(), db, []string{"s", "primary", "owner", ""})
			}
			if err != nil || status != store.StatusOK || len(cells) != 1 {
				t.Fatal(running, repeat, status, cells, err)
			}
			var history []struct {
				Role   string            `json:"role"`
				Blocks []json.RawMessage `json:"blocks"`
			}
			if json.Unmarshal([]byte(cells[0]), &history) != nil || len(history) != 3 || history[0].Role != "user" || history[1].Role != "assistant" || history[2].Role != "tool" {
				t.Fatal("causal history order lost", running, repeat, cells)
			}
			if !strings.Contains(string(history[1].Blocks[0]), "0.123456789012345678901234567890") || !strings.Contains(string(history[2].Blocks[0]), "actual result") {
				t.Fatal("original call/result changed", cells)
			}
		}
	}
}

func TestSessionIRHistoryBoundsUnmatchedStreamStarts(t *testing.T) {
	db := newSessDB()
	events := [][2]string{{"active", runningStart}, {"active", runningUser}}
	for i := 0; i < 4097; i++ {
		events = append(events, [2]string{"active", fmt.Sprintf(`{"kind":"delta","message_id":"stream-%d","role":"assistant","payload":{"type":"TURN_START"}}`, i)})
	}
	db.rows = runningHistoryRows(events...)
	_, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
	if err == nil || len(cells) != 0 || !strings.Contains(err.Error(), "capacity") {
		t.Fatal("unbounded unmatched stream order", cells, err)
	}
}

func TestSessionIRHistoryCheckpointResetsStreamObservationOrder(t *testing.T) {
	const oldStart = `{"kind":"delta","message_id":"reused-id","role":"assistant","payload":{"type":"TURN_START"}}`
	const checkpointMessage = `{"kind":"history","payload":{"scope":"primary","phase":"message","index":0,"message":{"role":"user","blocks":[{"type":"text","text":"checkpoint"}]}}}`
	const checkpointCommit = `{"kind":"history","payload":{"scope":"primary","phase":"commit","count":1}}`
	const first = `{"kind":"message","message_id":"new-id","payload":{"role":"assistant","blocks":[{"type":"text","text":"first after checkpoint"}]}}`
	const second = `{"kind":"message","message_id":"reused-id","payload":{"role":"assistant","blocks":[{"type":"text","text":"second after checkpoint"}]}}`
	db := newSessDB()
	db.rows = runningHistoryRows([2]string{"active", runningStart}, [2]string{"active", oldStart}, [2]string{"checkpoint", checkpointMessage}, [2]string{"checkpoint", checkpointCommit}, [2]string{"active", first}, [2]string{"active", second})
	status, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal(status, cells, err)
	}
	if strings.Index(cells[0], "first after checkpoint") > strings.Index(cells[0], "second after checkpoint") || !strings.Contains(cells[0], "checkpoint") {
		t.Fatal("old observed position crossed checkpoint", cells)
	}
}

func TestSessionIRRunningHistoryRefusesCorruptAndUnreconciledSource(t *testing.T) {
	base := func() [][]any {
		return runningHistoryRows([2]string{"active", runningStart}, [2]string{"active", runningUser})
	}
	tests := map[string]func() [][]any{
		"gap":                   func() [][]any { r := base(); r[1][0] = "3"; return r },
		"noncanonical sequence": func() [][]any { r := base(); r[1][0] = "02"; return r },
		"foreign row actor":     func() [][]any { r := base(); r[1][2] = "other"; return r },
		"invalid event": func() [][]any {
			r := base()
			r[1][4] = `{"kind":"message","message_id":"x","payload":{"role":"operator","blocks":[]}}`
			return r
		},
		"duplicate fields": func() [][]any {
			r := base()
			r[1][4] = `{"kind":"message","kind":"execution","payload":{"status":"running"}}`
			return r
		},
		"other open turn": func() [][]any {
			return runningHistoryRows([2]string{"active", runningStart}, [2]string{"other", runningStart})
		},
		"orphan observations": func() [][]any {
			return runningHistoryRows([2]string{"active", runningStart}, [2]string{"other", runningUser})
		},
		"uncommitted checkpoint": func() [][]any {
			return runningHistoryRows([2]string{"active", runningStart}, [2]string{"checkpoint", `{"kind":"history","payload":{"scope":"primary","phase":"message","index":0,"message":{"role":"user","blocks":[{"type":"text","text":"pending"}]}}}`})
		},
	}
	for name, rows := range tests {
		t.Run(name, func(t *testing.T) {
			db := newSessDB()
			db.rows = rows()
			status, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
			if err == nil || len(cells) != 0 {
				t.Fatal("source refusal became history", status, cells, err)
			}
		})
	}
}

func TestSessionIRRunningHistoryRequiresCurrentRunningTurn(t *testing.T) {
	for _, rows := range [][][]any{nil, runningHistoryRows([2]string{"active", runningStart}, [2]string{"active", runningUser}, [2]string{"active", runningDone})} {
		db := newSessDB()
		db.rows = rows
		status, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
		if err != nil || status != store.StatusMissing || len(cells) != 0 {
			t.Fatal(status, cells, err)
		}
	}
	for _, fields := range [][]string{{"", "owner", "active"}, {"s", "", "active"}, {"s", "owner", ""}} {
		db := newSessDB()
		status, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, fields)
		if err != nil || status != store.StatusInvalid || len(cells) != 0 || len(db.executed) != 0 {
			t.Fatal(status, cells, err, db.executed)
		}
	}
}

func TestSessionIRLiveRunningHistoryOwnerSnapshotAndCheckpoint(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_running_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql"} {
		sql, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(sql)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('s','owner')`); err != nil {
		t.Fatal(err)
	}
	appendEvent := func(id, turn, payload string) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{"s", id, turn, "owner", payload})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
		var stored string
		if err = pool.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id='s' AND event_id=$1`, id).Scan(&stored); err != nil || !validSessionIREvent([]byte(stored)) {
			t.Fatalf("stored event %s failed admission: %s (%v)", id, stored, err)
		}

	}
	snapshot := func(actor string) (uint32, string, error) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRRunningHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{"s", actor, "active"})
		if len(cells) == 1 {
			return status, cells[0], err
		}
		return status, "", err
	}
	appendEvent("old:start", "old", runningStart)
	appendEvent("old:user", "old", runningUser)
	appendEvent("old:done", "old", runningDone)
	appendEvent("active:start", "active", runningStart)
	appendEvent("active:user", "active", runningUser)
	status, wire, err := snapshot("owner")
	if err != nil || status != store.StatusOK || strings.Count(wire, "current user") != 2 {
		t.Fatal(status, wire, err)
	}
	if status, wire, err = snapshot("other"); err != nil || status != store.StatusMissing || wire != "" {
		t.Fatal("foreign source exposed", status, wire, err)
	}
	// A host checkpoint includes the admitted seed. Observations incorporated by
	// it must not be appended again while the execution remains open.
	seed := `[{"role":"user","blocks":[{"type":"text","text":"checkpoint seed"}]}]`
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err = sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{"s", "seed", "primary", "owner", seed})
	if err != nil || status != store.StatusOK {
		tx.Rollback(ctx)
		t.Fatal(status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if status, wire, err = snapshot("owner"); err != nil || status != store.StatusOK || wire != seed {
		t.Fatal("checkpoint duplicated observations", status, wire, err)
	}
	appendEvent("active:answer", "active", `{"kind":"message","message_id":"answer","payload":{"role":"assistant","blocks":[{"type":"text","text":"actual answer"}]}}`)
	if status, wire, err = snapshot("owner"); err != nil || status != store.StatusOK || strings.Count(wire, "actual answer") != 1 || strings.Contains(wire, "current user") {
		t.Fatal(status, wire, err)
	}
	appendEvent("active:model-start", "active", `{"kind":"delta","message_id":"streamed-call","role":"assistant","payload":{"type":"TURN_START"}}`)
	appendEvent("active:call-start", "active", `{"kind":"delta","message_id":"streamed-call","role":"assistant","payload":{"type":"BLOCK_START","block_id":0,"kind":"tool_use","tool_id":"original-call","namespace":"aimee","name":"read_file","input":{}}}`)
	encodedArguments, err := json.Marshal(`{"value":0.123456789012345678901234567890}`)
	if err != nil {
		t.Fatal(err)
	}
	appendEvent("active:call-arguments", "active", `{"kind":"delta","message_id":"streamed-call","role":"assistant","payload":{"type":"BLOCK_DELTA","block_id":0,"kind":"tool_use","tool_args_delta":`+string(encodedArguments)+`}}`)
	appendEvent("active:call-ready", "active", `{"kind":"delta","message_id":"streamed-call","role":"assistant","payload":{"type":"BLOCK_STOP","block_id":0,"kind":"tool_use"}}`)
	appendEvent("active:call-result", "active", `{"kind":"message","message_id":"actual-call-result","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"original-call","content":"verified result","is_error":false}]}}`)
	appendEvent("active:model-stop", "active", `{"kind":"delta","message_id":"streamed-call","role":"assistant","payload":{"type":"TURN_STOP"}}`)
	appendEvent("active:model-snapshot", "active", `{"kind":"message","message_id":"streamed-call","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"original-call","namespace":"aimee","name":"read_file","input":{"value":0.123456789012345678901234567890}}]}}`)
	status, wire, err = snapshot("owner")
	if err != nil || status != store.StatusOK || !strings.Contains(wire, "0.123456789012345678901234567890") || strings.Index(wire, "tool_use") > strings.Index(wire, "tool_result") {
		t.Fatal("live early tool result reordered before source", status, wire, err)
	}
	appendEvent("active:done", "active", runningDone)
	if status, wire, err = snapshot("owner"); err != nil || status != store.StatusMissing || wire != "" {
		t.Fatal("closed source admitted", status, wire, err)
	}
}

func TestCanonicalHistoryBudgetsAreOperationSpecific(t *testing.T) {
	history := map[uint32]bool{11: true, 15: true, 33: true, 35: true, 37: true, 41: true, 44: true, 45: true, 50: true, 53: true}
	for id, op := range Sessions.Ops {
		if history[id] {
			if op.Timeout != store.CanonicalHistoryTimeout {
				t.Fatalf("operation %d missing bounded history budget", id)
			}
		} else if op.Timeout != 0 {
			t.Fatalf("unrelated operation %d changed ordinary budget", id)
		}
	}
	if !Sessions.Ops[33].Tx || !Sessions.Ops[35].Tx {
		t.Fatal("paged source read lost enclosing owner transaction")
	}
}

func TestDelegateSourceBudgetIsOperationSpecific(t *testing.T) {
	for id, op := range Telemetry.Ops {
		if id == opDelegateExecutionExperienceRecord {
			if op.Timeout != store.CanonicalHistoryTimeout {
				t.Fatal("delegate source budget missing")
			}
		} else if op.Timeout != 0 {
			t.Fatalf("unrelated telemetry op %d budget changed", id)
		}
	}
}
