package families

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

type historyBatchInsertFault struct {
	store.Queryer
	calls int
}

func (q *historyBatchInsertFault) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	if sql == sessionIRHistoryInsertBatchSQL {
		q.calls++
		if q.calls == 2 {
			return nil, errors.New("controlled second chunk failure")
		}
	}
	return q.Queryer.Query(ctx, sql, args...)
}

func TestInputHistoryBatchLiveAtomicReplayWideSequencesAndConcurrency(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	schema := fmt.Sprintf("history_batch_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql"} {
		ddl, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(ddl)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('wide','actor'),('rollback','actor'),('concurrent','actor'),('fallback','actor')`); err != nil {
		t.Fatal(err)
	}
	const base = int64(9007199254740993)
	if _, err = pool.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES ('wide',$1,'seed','seed','actor','{}')`, base); err != nil {
		t.Fatal(err)
	}
	invoke := func(session, checkpoint, actor, messages string) (uint32, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, err
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRHistorySaveInputAuthorized(ctx, sessionIRLiveTx{tx}, []string{session, checkpoint, "primary", actor, messages}, true, true)
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, err
	}
	messages := batchHistoryMessages(t, 8)
	status, err := invoke("wide", "checkpoint", "actor", messages)
	if err != nil || status != store.StatusOK {
		t.Fatalf("wide append: %d %v", status, err)
	}
	var count int
	var last int64
	if err = pool.QueryRow(ctx, `SELECT count(*),max(sequence) FROM session_ir_events WHERE session_id='wide'`).Scan(&count, &last); err != nil || count != 10 || last != base+9 {
		t.Fatalf("wide exact sequence count=%d max=%d err=%v", count, last, err)
	}
	if _, err = pool.Exec(ctx, `INSERT INTO session_native_recovery_fences VALUES ('wide','checkpoint','actor','owner','claim','containment',1)`); err != nil {
		t.Fatal(err)
	}
	status, err = invoke("wide", "checkpoint", "actor", messages)
	if err != nil || status != store.StatusOK {
		t.Fatal("exact replay must survive later fence", status, err)
	}
	status, err = invoke("wide", "checkpoint", "actor", batchHistoryMessages(t, 7))
	if err != nil || status != store.StatusInvalid {
		t.Fatal("conflict must reject", status, err)
	}
	status, err = invoke("wide", "new", "foreign", messages)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("foreign actor must reject", status, err)
	}
	// A failure after the first real INSERT must roll back the entire checkpoint.
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	fault := &historyBatchInsertFault{Queryer: sessionIRLiveTx{tx}}
	_, _, err = sessionIRHistorySaveInputAuthorized(ctx, fault, []string{"rollback", "checkpoint", "primary", "actor", batchHistoryMessages(t, 140)}, true, true)
	if err == nil || fault.calls != 2 {
		tx.Rollback(ctx)
		t.Fatal("did not reach controlled second-chunk outage", err, fault.calls)
	}
	if err = tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='rollback'`).Scan(&count); err != nil || count != 0 {
		t.Fatal("rollback left partial checkpoint", count, err)
	}
	// Distinct checkpoints contend on the same owner row, allocating one gapless sequence.
	var wg sync.WaitGroup
	failures := make(chan error, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			s, e := invoke("concurrent", fmt.Sprintf("checkpoint-%d", i), "actor", messages)
			if e != nil || s != store.StatusOK {
				failures <- fmt.Errorf("status %d: %v", s, e)
			}
		}(i)
	}
	wg.Wait()
	close(failures)
	for e := range failures {
		t.Error(e)
	}
	var first int64
	if err = pool.QueryRow(ctx, `SELECT count(*),min(sequence),max(sequence) FROM session_ir_events WHERE session_id='concurrent'`).Scan(&count, &first, &last); err != nil || count != 72 || first != 1 || last != 72 {
		t.Fatal("concurrent sequence allocation", count, first, last, err)
	}
	// Successful multi-chunk publication and exact replay exercise the same production SQL.
	large := batchHistoryMessages(t, 140)
	status, err = invoke("rollback", "successful", "actor", large)
	if err != nil || status != store.StatusOK {
		t.Fatal("multi-chunk append", status, err)
	}
	status, err = invoke("rollback", "successful", "actor", large)
	if err != nil || status != store.StatusOK {
		t.Fatal("multi-chunk replay", status, err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='rollback'`).Scan(&count); err != nil || count != 141 {
		t.Fatal("multi-chunk row count", count, err)
	}
	// JSON string escaping can exceed the batch envelope while the event remains
	// valid. The original single-event path must preserve that event exactly.
	fallback, err := json.Marshal([]any{map[string]any{"role": "user", "blocks": []any{map[string]any{"type": "text", "text": strings.Repeat("\"", 270000)}}}})
	if err != nil {
		t.Fatal(err)
	}
	status, err = invoke("fallback", "oversized-envelope", "actor", string(fallback))
	if err != nil || status != store.StatusOK {
		t.Fatal("single-event envelope fallback", status, err)
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='fallback'`).Scan(&count); err != nil || count != 2 {
		t.Fatal("fallback row count", count, err)
	}

}
