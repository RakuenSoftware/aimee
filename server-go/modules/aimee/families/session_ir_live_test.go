package families

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql"} {
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
	status, cells, err := sessionIRPage(ctx, q, []string{"s", "0", "128"})
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
	status, cells, err = sessionIRPage(ctx, q, []string{"other", "0", "128"})
	if err != nil || status != store.StatusOK || cells[0] != "[]" {
		t.Fatalf("foreign session inherited events: %v %v", cells, err)
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
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
}
