package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestSessionIRLiveHistoryCheckpointOwnershipIsolationAndAtomicReload(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_history_%d", time.Now().UnixNano())
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
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('s','alice')`); err != nil {
		t.Fatal(err)
	}
	save := func(id, scope, actor, history string, commit bool) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{"s", id, scope, actor, history})
		if commit {
			if err != nil || status != store.StatusOK {
				t.Fatalf("save: %d %v", status, err)
			}
			if err = tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		} else if err == nil && status == store.StatusOK {
			t.Fatal("unauthorized or malformed history accepted")
		}
	}
	load := func(scope, actor string) (uint32, string) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{"s", scope, actor, "turn"})
		if err != nil {
			t.Fatal(err)
		}
		if status == store.StatusOK {
			return status, cells[0]
		}
		return status, ""
	}
	base := `[{"role":"user","blocks":[{"type":"text","text":"keep constraints"}]}]`
	save("initial", "primary", "alice", base, true)
	if status, got := load("primary", "alice"); status != store.StatusOK || got != base {
		t.Fatalf("reload: %d %s", status, got)
	}
	// A parent owns only the execution reference. The expandable conversation
	// is paged from the child's separately authorized log and never joins the prompt.
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('private-child','alice')`); err != nil {
		t.Fatal(err)
	}
	referenceTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer referenceTx.Rollback(ctx)
	for i, event := range []struct{ session, payload string }{
		{"s", `{"kind":"child_execution","payload":{"session_id":"private-child","turn_id":"child-run","role":"review","status":"running"}}`},
		{"private-child", `{"kind":"execution","payload":{"status":"running"}}`},
		{"private-child", `{"kind":"message","message_id":"child-answer","payload":{"role":"assistant","blocks":[{"type":"text","text":"PRIVATE_CHILD_LOG"}]}}`},
		{"private-child", `{"kind":"execution","payload":{"status":"completed"}}`},
		{"s", `{"kind":"child_execution","payload":{"session_id":"private-child","turn_id":"child-run","role":"review","status":"completed"}}`},
	} {
		status, _, appendErr := sessionIRAppend(ctx, sessionIRLiveTx{referenceTx}, []string{event.session, fmt.Sprintf("ref-%d", i), "child-run", "alice", event.payload})
		if appendErr != nil || status != store.StatusOK {
			t.Fatalf("child/reference append: %d %v", status, appendErr)
		}
	}
	if err = referenceTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if _, got := load("primary", "alice"); got != base {
		t.Fatalf("child reference changed parent prompt: %s", got)
	}
	pageTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer pageTx.Rollback(ctx)
	for _, check := range []struct {
		session, actor string
		private        bool
	}{
		{"s", "alice", false}, {"private-child", "alice", true}, {"private-child", "mallory", false},
	} {
		status, cells, pageErr := sessionIRPage(ctx, sessionIRLiveTx{pageTx}, []string{check.session, "0", "128", check.actor})
		if pageErr != nil || status != store.StatusOK || len(cells) != 1 || strings.Contains(cells[0], "PRIVATE_CHILD_LOG") != check.private {
			t.Fatalf("child log isolation: %s %s %v %v", check.session, check.actor, cells, pageErr)
		}
	}

	save("child", "delegate:child", "alice", `[{"role":"user","blocks":[{"type":"text","text":"PRIVATE_CHILD"}]}]`, true)
	if _, got := load("primary", "alice"); strings.Contains(got, "PRIVATE_CHILD") || got != base {
		t.Fatalf("child leaked: %s", got)
	}
	if _, got := load("delegate:child", "alice"); !strings.Contains(got, "PRIVATE_CHILD") {
		t.Fatal("child scope lost")
	}
	save("forged", "primary", "mallory", base, false)
	if status, _ := load("primary", "mallory"); status != store.StatusMissing {
		t.Fatal("cross-owner load succeeded")
	}
	save("malformed", "primary", "alice", `[{"role":"assistant","blocks":[{"type":"made_up"}]}]`, false)
	appendEvent := func(id, turn, payload string) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		if status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{"s", id, turn, "alice", payload}); err != nil || status != store.StatusOK {
			t.Fatalf("append: %d %v", status, err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	appendEvent("running", "turn", "{\"kind\":\"execution\",\"payload\":{\"status\":\"running\"}}")
	appendEvent("current-user", "turn", `{"kind":"message","message_id":"user","payload":{"role":"user","blocks":[{"type":"text","text":"CURRENT_PROMPT"}]}}`)
	if _, got := load("primary", "alice"); strings.Contains(got, "CURRENT_PROMPT") {
		t.Fatal("active browser prompt would be duplicated")
	}
	// A fresh process/turn cannot silently replay an unfinished execution.
	{
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		_, _, err = sessionIRHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{"s", "primary", "alice", "new-turn"})
		_ = tx.Rollback(ctx)
		if err == nil {
			t.Fatal("unfinished execution was silently replayed")
		}
	}
	appendEvent("final", "turn", `{"kind":"execution","payload":{"status":"completed"}}`)
	if _, got := load("primary", "alice"); !strings.Contains(got, "CURRENT_PROMPT") {
		t.Fatal("completed native history not projected")
	}
	// Governance/compaction checkpoint supersedes observations; execution-stop cannot restore them.
	appendEvent("denied-observation", "governed", `{"kind":"message","message_id":"denied","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"denied-call","name":"Task","input":{}}]}}`)
	save("normalized", "primary", "alice", base, true)
	appendEvent("governed-stop", "governed", `{"kind":"execution","payload":{"status":"completed"}}`)
	if _, got := load("primary", "alice"); got != base {
		t.Fatalf("observation overwrote admitted history: %s", got)
	}
}

// Large history atomicity has its own deadline; smaller ownership and lifecycle
// checks cannot consume the reader/writer and exact-retry budget.
func TestSessionIRLiveLargeHistoryCheckpointAtomicReloadAndRetry(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_history_%d", time.Now().UnixNano())
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
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('s','alice')`); err != nil {
		t.Fatal(err)
	}
	save := func(id, scope, actor, history string, commit bool) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{"s", id, scope, actor, history})
		if commit {
			if err != nil || status != store.StatusOK {
				t.Fatalf("save: %d %v", status, err)
			}
			if err = tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		} else if err == nil && status == store.StatusOK {
			t.Fatal("unauthorized or malformed history accepted")
		}
	}
	load := func(scope, actor string) (uint32, string) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{"s", scope, actor, "turn"})
		if err != nil {
			t.Fatal(err)
		}
		if status == store.StatusOK {
			return status, cells[0]
		}
		return status, ""
	}
	// Preserve the committed source that the original reader/writer scenario
	// superseded; this is a checkpoint replacement, not initial publication.
	save("initial", "primary", "alice", `[{"role":"user","blocks":[{"type":"text","text":"keep constraints"}]}]`, true)
	// Each event fits the log contract; the complete history exceeds the old 1 MiB load ceiling.
	large := []map[string]any{{"role": "user", "blocks": []map[string]any{{"type": "text", "text": strings.Repeat("x", 700000)}}}, {"role": "assistant", "blocks": []map[string]any{{"type": "text", "text": strings.Repeat("y", 700000)}}}}
	encoded, _ := json.Marshal(large)
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{"s", "large", "primary", "alice", string(encoded)}); err != nil || status != store.StatusOK {
		t.Fatalf("large checkpoint: %d %v", status, err)
	}
	// Paged history owns a consistent source view: a distinct reader must wait
	// for the checkpoint writer's session lock, then see its complete commit.
	type readAnswer struct {
		status uint32
		cells  []string
		err    error
	}
	readerPID := make(chan uint32, 1)
	readerDone := make(chan readAnswer, 1)
	go func() {
		reader, e := pool.Begin(ctx)
		if e != nil {
			readerDone <- readAnswer{err: e}
			return
		}
		defer reader.Rollback(ctx)
		readerPID <- reader.Conn().PgConn().PID()
		status, cells, e := sessionIRHistoryLoad(ctx, sessionIRLiveTx{reader}, []string{"s", "primary", "alice", "turn"})
		readerDone <- readAnswer{status, cells, e}
	}()
	var backend uint32
	select {
	case backend = <-readerPID:
	case answer := <-readerDone:
		t.Fatalf("concurrent reader could not start: %v", answer.err)
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	waitCtx, stopWait := context.WithTimeout(ctx, 5*time.Second)
	defer stopWait()
	tick := time.NewTicker(5 * time.Millisecond)
	defer tick.Stop()
	for {
		var waiting bool
		if err := pool.QueryRow(waitCtx, `SELECT EXISTS(SELECT 1 FROM pg_stat_activity WHERE pid=$1 AND wait_event_type='Lock')`, backend).Scan(&waiting); err != nil {
			t.Fatal("observe concurrent history row-lock wait", err)
		}
		if waiting {
			break
		}
		select {
		case answer := <-readerDone:
			t.Fatalf("reader returned before checkpoint commit: status=%d err=%v", answer.status, answer.err)
		case <-tick.C:
		case <-waitCtx.Done():
			t.Fatal("history reader never reached actual PostgreSQL lock wait", waitCtx.Err())
		}
	}
	select {
	case answer := <-readerDone:
		t.Fatalf("reader returned uncommitted checkpoint: status=%d err=%v", answer.status, answer.err)
	default:
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case answer := <-readerDone:
		if answer.err != nil || answer.status != store.StatusOK || len(answer.cells) != 1 || answer.cells[0] != string(encoded) {
			t.Fatalf("concurrent reader did not see whole committed checkpoint: status=%d cells=%d err=%v", answer.status, len(answer.cells), answer.err)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if _, got := load("primary", "alice"); got != string(encoded) {
		t.Fatalf("large reload differs (%d vs %d bytes)", len(got), len(encoded))
	}
	// Retrying the same checkpoint is exact and adds no second copy.
	save("large", "primary", "alice", string(encoded), true)
	if _, got := load("primary", "alice"); got != string(encoded) {
		t.Fatal("checkpoint retry duplicated history")
	}
}
