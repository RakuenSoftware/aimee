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
		rows[i] = []any{fmt.Sprint(i + 1), event[0], "owner", event[1]}
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
	if db.executed[0] != sessionIRRunningHistorySQL || fmt.Sprint(db.args[0]) != "[s owner]" {
		t.Fatal("owner query changed", db.executed, db.args)
	}
	// Completed-history callers retain their original contract: the open turn is
	// permitted by the active ID but excluded from the returned conversation.
	legacy := newSessDB()
	for _, row := range db.rows {
		legacy.rows = append(legacy.rows, []any{row[1], row[3]})
	}
	status, cells, err = sessionIRHistoryLoad(context.Background(), legacy, []string{"s", "primary", "owner", "active"})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal(status, cells, err)
	}
	if err = json.Unmarshal([]byte(cells[0]), &history); err != nil || len(history) != 1 {
		t.Fatal("legacy pending turn leaked", cells, err)
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
			r[1][3] = `{"kind":"message","message_id":"x","payload":{"role":"operator","blocks":[]}}`
			return r
		},
		"duplicate fields": func() [][]any {
			r := base()
			r[1][3] = `{"kind":"message","kind":"execution","payload":{"status":"running"}}`
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql"} {
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
	appendEvent("active:done", "active", runningDone)
	if status, wire, err = snapshot("owner"); err != nil || status != store.StatusMissing || wire != "" {
		t.Fatal("closed source admitted", status, wire, err)
	}
}
