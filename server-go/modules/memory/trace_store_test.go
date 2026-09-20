package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func exerciseTraceStoreReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT trace_store_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT trace_store_replay; RELEASE SAVEPOINT trace_store_replay`) }()
	exec(`RESET ROLE; TRUNCATE trace_mining_log; SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L0','procedure','recovery:trace-read->trace-search','private','project','trace-private')`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	call := func(op string, batch *traceMiningBatch) (map[string]any, bus.ModuleStatus) {
		t.Helper()
		raw, _ := json.Marshal(map[string]any{"operation": op, "batch": batch, "project": "trace-visible", "session_id": "trace-session"})
		frame, _ := bus.EncodeCommand("runtime", raw)
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		body, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var result map[string]any
		if err = json.Unmarshal(body, &result); err != nil {
			t.Fatal(err)
		}
		return result, status
	}
	state, status := call("trace-state", nil)
	if status != bus.ModuleStatusOK || state["last_id"] != "0" {
		t.Fatal(state, status)
	}
	const start int64 = 9007199254741507
	row := func(offset, plan, turn int64, tool, result string) traceMiningRow {
		return traceMiningRow{start + offset, turn, traceObservation{plan, tool, result}}
	}
	batch := &traceMiningBatch{Rows: []traceMiningRow{
		row(1, 2, 1, "trace-read", "error"), row(2, 1, 1, "trace-read", "error"), row(3, 2, 2, "trace-read", "error"),
		row(4, 1, 2, "trace-search", "ok"), row(5, 2, 3, "trace-read", "error"), row(6, 2, 4, "trace-search", "ok"),
	}}
	result, status := call("trace-apply", batch)
	if status != bus.ModuleStatusOK || result["status"] != "ok" || result["emitted"] != float64(2) || result["last_id"] != strconv.FormatInt(start+6, 10) {
		t.Fatal(result, status)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories m JOIN memory_fact_actors a ON a.memory_id=m.id JOIN kb_async_jobs j ON j.kind='memory_facts' AND j.document_id=m.id
 WHERE m.key='recovery:trace-read->trace-search' AND m.scope_type='project' AND m.scope_value='trace-visible' AND m.source_session='trace-session'
 AND m.provenance_category='agent_message' AND m.confidence=.7 AND a.actor_role='model'`).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	result, status = call("trace-apply", batch)
	if status != bus.ModuleStatusOK || result["status"] != "error" {
		t.Fatal("stale batch accepted", result, status)
	}
	// Fail the final cursor insert after both the anti-pattern and canonical
	// memory/actor/job writes. Every write must roll back with the failed cursor.
	exec(`RESET ROLE; CREATE FUNCTION pg_temp.reject_trace_cursor() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'cursor fixture failure'; END $$;
 CREATE TRIGGER reject_trace_cursor BEFORE INSERT ON trace_mining_log FOR EACH ROW EXECUTE FUNCTION pg_temp.reject_trace_cursor(); SET LOCAL ROLE aimee_store_runtime`)
	failed := &traceMiningBatch{AfterID: start + 6, Rows: []traceMiningRow{
		row(7, 3, 1, "trace-fail", "error"), row(8, 3, 2, "trace-fail", "error"), row(9, 3, 3, "trace-fail", "error"), row(10, 3, 4, "trace-recover", "ok"),
	}}
	if _, status = call("trace-apply", failed); status != bus.ModuleStatusInternal {
		t.Fatal("failed cursor accepted", status)
	}
	if err := tx.QueryRow(ctx, `SELECT (SELECT count(*) FROM memories WHERE key='recovery:trace-fail->trace-recover')+(SELECT count(*) FROM anti_patterns WHERE pattern LIKE 'Retry loop: trace-fail%')`).Scan(&count); err != nil || count != 0 {
		t.Fatal("partial finding committed", count, err)
	}
	state, status = call("trace-state", nil)
	if status != bus.ModuleStatusOK || state["last_id"] != strconv.FormatInt(start+6, 10) {
		t.Fatal(state, status)
	}
	exec(`RESET ROLE; DROP TRIGGER reject_trace_cursor ON trace_mining_log; SET LOCAL ROLE aimee_store_runtime`)
	result, status = call("trace-apply", failed)
	if status != bus.ModuleStatusOK || result["emitted"] != float64(2) {
		t.Fatal(result, status)
	}
	// Advancing with the same patterns leaves their canonical records untouched.
	for i := range failed.Rows {
		failed.Rows[i].ID += 4
	}
	failed.AfterID = start + 10
	result, status = call("trace-apply", failed)
	if status != bus.ModuleStatusOK || result["emitted"] != float64(0) {
		t.Fatal(result, status)
	}
	failed.AfterID = start + 14
	failed.Rows = []traceMiningRow{}
	result, status = call("trace-apply", failed)
	if status != bus.ModuleStatusOK || result["emitted"] != float64(0) {
		t.Fatal(result, status)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM trace_mining_log`).Scan(&count); err != nil || count != 3 {
		t.Fatal(count, err)
	}
	// Read failure is not an empty cursor or permission to start from zero.
	exec(`RESET ROLE; REVOKE SELECT ON trace_mining_log FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	if _, status = call("trace-state", nil); status != bus.ModuleStatusInternal {
		t.Fatal(status)
	}
	exec(`RESET ROLE; GRANT SELECT ON trace_mining_log TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	for _, bad := range []*traceMiningBatch{nil, {AfterID: -1, Rows: []traceMiningRow{}}, {AfterID: start, Rows: []traceMiningRow{row(2, 1, 1, "a", ""), row(1, 1, 2, "b", "")}}} {
		if _, status = call("trace-apply", bad); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("bad trace batch", bad, status)
		}
	}
	body := dataRequest(t, DataRequest{Operation: "trace-state"})
	if _, status = handler(bus.ModuleInvocation{StageID: StageData, PrincipalRef: 99}, body); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("peer read host cursor", status)
	}
}

func TestTraceBatchValidation(t *testing.T) {
	for _, input := range []string{`{"after_id":1,"rows":[]}`, `{"after_id":"0","rows":[{"id":1}]}`} {
		var batch traceMiningBatch
		if err := json.Unmarshal([]byte(input), &batch); err == nil {
			t.Fatal("numeric int64 accepted", input)
		}
	}
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"trace-state"}`))
		_, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status == bus.ModuleStatusOK {
			t.Fatal(fmt.Sprint(placement), "invented cursor")
		}
	}
}

func TestTraceStoreConcurrentPostgres(t *testing.T) {
	url := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if url == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL to packaged-schema PostgreSQL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	first, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close(context.Background())
	second, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close(context.Background())
	a, err := first.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Rollback(context.Background())
	b, err := second.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Rollback(context.Background())
	owner := &postgresDataStore{db: evalQueryer{a}, placement: PlacementKB}
	cursor, err := owner.traceCursor(ctx)
	if err != nil {
		t.Fatal(err)
	}
	batch := &traceMiningBatch{AfterID: cursor, Rows: []traceMiningRow{{ID: cursor + 1, Turn: 1, traceObservation: traceObservation{PlanID: 1, Tool: "concurrency-probe", Result: "ok"}}}}
	req := DataRequest{TraceBatch: batch, Scope: Scope{Type: ScopeGlobal, Value: "_global"}}
	if result, err := owner.applyTraceBatch(ctx, req); err != nil || result["status"] != "ok" {
		t.Fatal(result, err)
	}
	var logID int64
	if err = a.QueryRow(ctx, `SELECT id FROM trace_mining_log WHERE last_trace_id=$1 ORDER BY id DESC LIMIT 1`, cursor+1).Scan(&logID); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_, err := first.Exec(context.Background(), `RESET ROLE;`)
		if err == nil {
			_, err = first.Exec(context.Background(), `DELETE FROM trace_mining_log WHERE id=$1`, logID)
		}
		if err != nil {
			t.Error(err)
		}
	}()
	type outcome struct {
		result map[string]any
		err    error
	}
	done := make(chan outcome, 1)
	go func() {
		result, err := (&postgresDataStore{db: evalQueryer{b}, placement: PlacementKB}).applyTraceBatch(ctx, req)
		done <- outcome{result, err}
	}()
	select {
	case result := <-done:
		t.Fatal("concurrent apply bypassed open cursor transaction", result)
	case <-time.After(50 * time.Millisecond):
	}
	if err = a.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case result := <-done:
		if result.err != nil || result.result["status"] != "error" || result.result["kind"] != "conflict" {
			t.Fatal(result)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
}
