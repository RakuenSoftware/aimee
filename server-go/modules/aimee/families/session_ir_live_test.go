package families

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type sessionIRLiveTx struct{ pgx.Tx }

func (q sessionIRLiveTx) Exec(ctx context.Context, s string, a ...any) (store.Tag, error) {
	tag, err := q.Tx.Exec(ctx, s, a...)
	return store.RowsAffected(tag.RowsAffected()), err
}
func (q sessionIRLiveTx) Query(ctx context.Context, s string, a ...any) (store.Rows, error) {
	rows, err := q.Tx.Query(ctx, s, a...)
	if err != nil {
		return nil, err
	}
	return liveRows{rows}, nil
}
func (q sessionIRLiveTx) QueryRow(ctx context.Context, s string, a ...any) store.Row {
	return sessionIRLiveRow{q.Tx.QueryRow(ctx, s, a...)}
}

type sessionIRLiveRow struct{ pgx.Row }

func (r sessionIRLiveRow) Scan(dest ...any) error {
	err := r.Row.Scan(dest...)
	if errors.Is(err, pgx.ErrNoRows) {
		return store.ErrNoRows
	}
	return err
}

// Real transactions prove concurrent sequence allocation and retry behavior;
// mocks cannot establish the row-lock and cascade semantics of PostgreSQL.
func TestSessionIRLiveConcurrentReplayAndDeletion(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_ir_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_session_harness.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('s','operator'),('other','other')`); err != nil {
		t.Fatal(err)
	}

	// Equal opaque IDs from different harnesses are distinct. Another session
	// cannot adopt a handle already bound to the same harness.
	for _, binding := range []struct {
		sid, harness string
		ok           bool
	}{{"s", "claude", true}, {"s", "codex", true}, {"other", "claude", false}} {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		status, _, err := sessionHarnessBind(ctx, sessionIRLiveTx{tx}, []string{binding.sid, binding.harness, "same-native-id", map[string]string{"s": "operator", "other": "other"}[binding.sid]})
		if binding.ok {
			if err != nil || status != store.StatusOK {
				tx.Rollback(ctx)
				t.Fatalf("bind: %d %v", status, err)
			}
			if err = tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		} else {
			if err == nil {
				t.Fatal("cross-session adoption succeeded")
			}
			tx.Rollback(ctx)
		}
	}
	payload := `{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[{"type":"text","text":"hello"}]}}`
	appendEvent := func(event string) (string, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return "", err
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{"s", event, "turn", "operator", payload})
		if err != nil {
			return "", err
		}
		if status != store.StatusOK || len(cells) != 1 {
			return "", fmt.Errorf("append status %d", status)
		}
		if err = tx.Commit(ctx); err != nil {
			return "", err
		}
		return cells[0], nil
	}
	const count = 16
	sequences := make(chan string, count)
	failures := make(chan error, count)
	var wg sync.WaitGroup
	for n := 0; n < count; n++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			seq, err := appendEvent(fmt.Sprintf("event-%d", n))
			if err != nil {
				failures <- err
			} else {
				sequences <- seq
			}
		}(n)
	}
	wg.Wait()
	close(failures)
	close(sequences)
	for err := range failures {
		t.Fatal(err)
	}
	seen := map[string]bool{}
	for seq := range sequences {
		if seen[seq] {
			t.Fatalf("duplicate sequence %s", seq)
		}
		seen[seq] = true
	}
	for n := 1; n <= count; n++ {
		if !seen[strconv.Itoa(n)] {
			t.Fatalf("missing sequence %d", n)
		}
	}
	first, err := appendEvent("event-0")
	if err != nil {
		t.Fatal(err)
	}
	retry, err := appendEvent("event-0")
	if err != nil || retry != first {
		t.Fatalf("retry changed sequence: %s -> %s (%v)", first, retry, err)
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	q := sessionIRLiveTx{tx}
	status, cells, err := sessionIRPage(ctx, q, []string{"s", "0", "128", "operator"})
	if err != nil || status != store.StatusOK {
		t.Fatalf("page: %d %v", status, err)
	}
	var page []struct {
		Sequence string `json:"sequence"`
	}
	if err = json.Unmarshal([]byte(cells[0]), &page); err != nil {
		t.Fatal(err)
	}
	if len(page) != count {
		t.Fatalf("replay has %d events", len(page))
	}
	for n, e := range page {
		if e.Sequence != strconv.Itoa(n+1) {
			t.Fatal("replay order changed")
		}
	}
	status, cells, err = sessionIRPage(ctx, q, []string{"other", "0", "128", "other"})
	if err != nil || status != store.StatusOK || cells[0] != "[]" {
		t.Fatalf("foreign session inherited events: %v %v", cells, err)
	}

	status, cells, err = sessionIRPage(ctx, q, []string{"s", "0", "128", "other"})
	if err != nil || status != store.StatusOK || cells[0] != "[]" {
		t.Fatal("foreign principal read the transcript")
	}
	status, _, err = sessionIRAppend(ctx, q, []string{"s", "forged", "turn", "other", payload})
	if err != nil || status != store.StatusInvalid {
		t.Fatal("foreign principal appended to session")
	}
	status, _, err = sessionHarnessGet(ctx, q, []string{"s", "claude", "other"})
	if err != nil || status != store.StatusMissing {
		t.Fatal("foreign principal read a resume handle")
	}
	status, _, err = sessionHarnessBind(ctx, q, []string{"s", "claude", "forged", "other"})
	if err != nil || status != store.StatusInvalid {
		t.Fatal("foreign principal changed a resume handle")
	}
	if _, err = q.Exec(ctx, `DELETE FROM server_sessions WHERE id='s'`); err != nil {
		t.Fatal(err)
	}
	var remaining int
	if err = q.QueryRow(ctx, `SELECT count(*) FROM session_ir_events`).Scan(&remaining); err != nil || remaining != 0 {
		t.Fatalf("events survived deletion: %d %v", remaining, err)
	}
	status, _, err = sessionIRAppend(ctx, q, []string{"s", "late", "turn", "operator", payload})
	if err != nil || status != store.StatusMissing {
		t.Fatalf("deleted session accepted append: %d %v", status, err)
	}

	if err = q.QueryRow(ctx, `SELECT count(*) FROM session_harness_handles`).Scan(&remaining); err != nil || remaining != 0 {
		t.Fatalf("harness metadata did not cascade: %d %v", remaining, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
}

// The source session lock serializes concurrent projection retries, while a
// missing projection can be reconstructed from the retained canonical event.
func TestSessionIRLiveExecutionObservationRecovery(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("execution_observation_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_telemetry.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('s','operator')`); err != nil {
		t.Fatal(err)
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{"s", "event", "turn", "operator", `{"kind":"execution","payload":{"status":"failed","error":"real source error"}}`})
	if err != nil || status != store.StatusOK {
		tx.Rollback(ctx)
		t.Fatalf("append: %d %v", status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	observation := `{"schema_version":1,"source":{"kind":"canonical_session_log","session_id":"s","sequence":"1","event_id":"event","turn_id":"turn","actor":"operator"},"observed_status":"failed","error":"real source error","verification":"ungraded"}`
	record := func(raw string) (uint32, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, err
		}
		defer tx.Rollback(ctx)
		status, _, err := interactionEventRecord(ctx, sessionIRLiveTx{tx}, []string{"s", "task_outcome", "system", raw, "observed"})
		if err != nil || status != store.StatusOK {
			return status, err
		}
		return status, tx.Commit(ctx)
	}
	// A producer cannot change source identity, status, diagnostic or grading.
	for _, change := range [][2]string{{`"sequence":"1"`, `"sequence":"2"`}, {`"event_id":"event"`, `"event_id":"foreign"`}, {`"actor":"operator"`, `"actor":"other"`}, {`"observed_status":"failed"`, `"observed_status":"completed"`}, {`"error":"real source error"`, `"error":"invented"`}, {`"verification":"ungraded"`, `"verification":"verified"`}} {
		status, err := record(strings.Replace(observation, change[0], change[1], 1))
		if err != nil || (status != store.StatusInvalid && status != store.StatusMissing) {
			t.Fatalf("forgery: %d %v", status, err)
		}
	}
	var wg sync.WaitGroup
	failures := make(chan error, 16)
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			status, err := record(observation)
			if err != nil || status != store.StatusOK {
				failures <- fmt.Errorf("record: %d %v", status, err)
			}
		}()
	}
	wg.Wait()
	close(failures)
	for err := range failures {
		t.Fatal(err)
	}
	var count int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE event_type='task_outcome'`).Scan(&count); err != nil || count != 1 {
		t.Fatalf("concurrent retry count %d: %v", count, err)
	}
	// Pruned/lost derived rows do not erase or mutate the source.
	if _, err = pool.Exec(ctx, `DELETE FROM interaction_events WHERE event_type='task_outcome'`); err != nil {
		t.Fatal(err)
	}
	if status, err = record(observation); err != nil || status != store.StatusOK {
		t.Fatalf("recovery: %d %v", status, err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE event_type='task_outcome'`).Scan(&count); err != nil || count != 1 {
		t.Fatalf("recovered count %d: %v", count, err)
	}
	if _, err = pool.Exec(ctx, `DELETE FROM server_sessions WHERE id='s'`); err != nil {
		t.Fatal(err)
	}
	if status, err = record(observation); err != nil || status != store.StatusMissing {
		t.Fatalf("deleted source accepted: %d %v", status, err)
	}
}
