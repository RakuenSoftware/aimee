package families

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestSessionToolSourceLiveAtomicCustodyReplayAndReload(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	schema := fmt.Sprintf("tool_source_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_session_harness.sql", "schema_learning_projection_outbox.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql"} {
		raw, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(raw)); err != nil {
			t.Fatal(err)
		}
	}
	run := func(op uint32, f ...string) (uint32, []string, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		defer tx.Rollback(ctx)
		status, cells, err := Sessions.Ops[op].Run(ctx, sessionIRLiveTx{tx}, f)
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, cells, err
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('s','owner')`); err != nil {
		t.Fatal(err)
	}
	for _, entry := range [][2]string{{"running", runningStart}, {"user", runningUser}, {"call", `{"kind":"message","message_id":"call-source","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"call","name":"memory_serve","input":{"view":"current_state"}}]}}`}} {
		status, _, err := run(28, "s", entry[0], "turn", "owner", entry[1])
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
	}
	exact := sourceResultEvent(t, "call", "exact native output α 9007199254740993")
	if status, _, err := run(28, "s", "result", "turn", "owner", exact); err != nil || status != store.StatusInvalid {
		t.Fatal("ordinary source accepted", status, err)
	}
	if status, _, err := run(54, "s", "foreign", "turn", "other", exact); err != nil || status == store.StatusOK {
		t.Fatal("foreign actor accepted", status, err)
	}
	if status, _, err := run(54, "s", "orphan", "turn", "owner", sourceResultEvent(t, "absent", "orphan")); err != nil || status == store.StatusOK {
		t.Fatal("orphan accepted", status, err)
	}
	status, first, err := run(54, "s", "result", "turn", "owner", exact)
	if err != nil || status != store.StatusOK || len(first) != 1 {
		t.Fatal("actual tool source refused", status, first, err)
	}
	status, retry, err := run(54, "s", "result", "turn", "owner", exact)
	if err != nil || status != store.StatusOK || fmt.Sprint(first) != fmt.Sprint(retry) {
		t.Fatal("retry changed", status, retry, err)
	}
	if status, _, err := run(54, "s", "duplicate", "turn", "owner", exact); err != nil || status == store.StatusOK {
		t.Fatal("second result accepted", status, err)
	}
	if status, _, err := run(54, "s", "result", "turn", "owner", strings.Replace(exact, "exact native output", "changed output", 1)); err != nil || status == store.StatusOK {
		t.Fatal("changed result admitted", status, err)
	}
	status, history, err := run(opSessionIRRunningHistoryLoad, "s", "owner", "turn")
	if err != nil || status != store.StatusOK || len(history) != 1 {
		t.Fatal(status, history, err)
	}
	var messages []struct {
		Blocks []struct {
			ToolSource json.RawMessage `json:"tool_source"`
			Content    json.RawMessage `json:"content"`
		} `json:"blocks"`
	}
	if err = json.Unmarshal([]byte(history[0]), &messages); err != nil {
		t.Fatal(err)
	}
	found := false
	for _, message := range messages {
		for _, block := range message.Blocks {
			if len(block.ToolSource) != 0 {
				found = true
				if !strings.Contains(string(block.Content), "9007199254740993") {
					t.Fatal("text changed")
				}
			}
		}
	}
	if !found {
		t.Fatal("durable source lost on history reload", history)
	}
	var persisted string
	var rows int
	if err = pool.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id='s' AND event_id='result'`).Scan(&persisted); err != nil || persisted != exact {
		t.Fatal("result and receipt were not atomic/exact", err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='s'`).Scan(&rows); err != nil || rows != 4 {
		t.Fatal("refusals/replay wrote", rows, err)
	}

	// Host-only55 preserves already-committed receipts; it cannot mint, strip,
	// alter, rebind or move a result into another actor/running turn.
	originalHistory := history[0]
	var typed []canonical.Message
	if !canonical.Decode([]byte(originalHistory), &typed) {
		t.Fatal("history decode")
	}
	for i := range typed {
		for j := range typed[i].Blocks {
			typed[i].Blocks[j].ToolSource = nil
		}
	}
	raw, _ := json.Marshal(typed)
	stripped := string(raw)
	changed := strings.Replace(originalHistory, `{\"opaque\":true}`, `{\"changed\":true}`, 1)
	if changed == originalHistory {
		t.Fatal("fixture did not alter receipt")
	}
	for _, request := range [][]string{
		{"s", "strip", "primary", "owner", "turn", stripped},
		{"s", "change", "primary", "owner", "turn", changed},
		{"s", "foreign-owner", "primary", "other", "turn", originalHistory},
		{"s", "foreign-turn", "primary", "owner", "other-turn", originalHistory},
	} {
		status, _, err = run(opSessionToolSourceHistorySave, request...)
		if request[4] == "other-turn" {
			if !errors.Is(err, errSessionIRUnfinishedExecution) {
				t.Fatal("wrong active turn did not refuse with exact reconciliation error", status, err)
			}
		} else if err != nil || status != store.StatusInvalid {
			t.Fatal("invalid source checkpoint admitted", status, err)
		}
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='s'`).Scan(&rows); err != nil || rows != 4 {
		t.Fatal("checkpoint refusals wrote", rows, err)
	}
	if status, _, err = run(opSessionIRHistorySave, "s", "ordinary-checkpoint", "primary", "owner", originalHistory); err != nil || status != store.StatusInvalid {
		t.Fatal("ordinary checkpoint acquired receipt", status, err)
	}
	// The actual C local history classifies result Context after the immutable
	// event emission. Native source identity must survive this presentation step.
	classified, parseErr := canonical.DecodeMessages([]byte(originalHistory))
	if parseErr != nil {
		t.Fatal(parseErr)
	}
	for i := range classified {
		for j := range classified[i].Blocks {
			if classified[i].Blocks[j].Type == "tool_result" {
				classified[i].Blocks[j].Context = &canonical.Context{Origin: 4, Authority: 5, Trust: 6, ModelVisible: true}
			}
		}
	}
	classifiedRaw, marshalErr := json.Marshal(classified)
	if marshalErr != nil {
		t.Fatal(marshalErr)
	}
	originalHistory = string(classifiedRaw)
	status, _, err = run(opSessionToolSourceHistorySave, "s", "preserved-checkpoint", "primary", "owner", "turn", originalHistory)
	if err != nil || status != store.StatusOK {
		t.Fatal("preserving host checkpoint refused", status, err)
	}
	var committedRows int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='s'`).Scan(&committedRows); err != nil || committedRows <= rows {
		t.Fatal("checkpoint not durable", committedRows, err)
	}
	status, _, err = run(opSessionToolSourceHistorySave, "s", "preserved-checkpoint", "primary", "owner", "turn", originalHistory)
	if err != nil || status != store.StatusOK {
		t.Fatal("exact checkpoint retry refused", status, err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='s'`).Scan(&rows); err != nil || rows != committedRows {
		t.Fatal("checkpoint retry wrote", rows, err)
	}
	status, reloaded, err := run(opSessionIRRunningHistoryLoad, "s", "owner", "turn")
	if err != nil || status != store.StatusOK || len(reloaded) != 1 || !strings.Contains(reloaded[0], `tool_source`) {
		t.Fatal("checkpoint source lost on reload", status, err)
	}
	t.Log("actual owned PG op54 atomically persisted exact tool result+source, rejected foreign/orphan/ordinary/duplicate/altered requests, idempotent retry and running history reload retained source")
}
