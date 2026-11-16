package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

type nativeRecoveryFault struct {
	store.Queryer
	at string
}

func (q nativeRecoveryFault) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	if strings.Contains(sql, q.at) {
		return nil, errors.New("injected native recovery storage failure")
	}
	return q.Queryer.Exec(ctx, sql, args...)
}
func TestSessionNativeLiveAtomicRecoveryAndWriterFence(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	schema := fmt.Sprintf("native_recovery_%d", time.Now().UnixNano())
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
		sql, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(sql)); err != nil {
			t.Fatal(err)
		}
	}
	invoke := func(op uint32, r nativeOwnerRequest) (uint32, []string, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		defer tx.Rollback(ctx)
		fn := sessionNativeOwnerClaim
		if op == opSessionNativeReconcile {
			fn = sessionNativeReconcile
		}
		status, cells, err := fn(ctx, sessionIRLiveTx{tx}, nativeRecoveryFields(r))
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, cells, err
	}
	appendEvent := func(r nativeOwnerRequest, id, raw string, verified bool) (uint32, []string, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		defer tx.Rollback(ctx)
		fn := sessionIRAppend
		if verified {
			fn = sessionIRAppendVerified
		}
		status, cells, err := fn(ctx, sessionIRLiveTx{tx}, []string{r.Session, id, r.Turn, "actor", raw})
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, cells, err
	}
	rawMessage := `{"kind":"message","message_id":"original","payload":{"role":"user","blocks":[{"type":"text","text":"retained original"}]}}`
	newClaim := func(name string) nativeOwnerRequest {
		t.Helper()
		r := nativeRecoveryFixture()
		r.Session = name
		sum := sha256.Sum256([]byte(name))
		r.Claim = hex.EncodeToString(sum[:16])
		sum = sha256.Sum256([]byte("owner:" + name))
		r.Owner = hex.EncodeToString(sum[:16])
		if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, r.Session); err != nil {
			t.Fatal(err)
		}
		for i, raw := range []string{`{"kind":"execution","payload":{"status":"running"}}`, rawMessage} {
			status, _, err := appendEvent(r, fmt.Sprint(i), raw, false)
			if err != nil || status != store.StatusOK {
				t.Fatal(status, err)
			}
		}
		status, _, err := invoke(opSessionNativeOwnerClaim, r)
		if err != nil || status != store.StatusOK {
			t.Fatal("register exact host owner", status, err)
		}
		status, _, err = invoke(opSessionNativeOwnerClaim, r)
		if err != nil || status != store.StatusOK {
			t.Fatal("owner retry", status, err)
		}
		r.Expected = r.token("pending")
		return r
	}
	count := func(sql string, args ...any) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, sql, args...).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	token := func(r nativeOwnerRequest) string {
		var s string
		if err := pool.QueryRow(ctx, `SELECT native_id FROM session_harness_handles WHERE session_id=$1 AND harness=$2`, r.Session, r.key()).Scan(&s); err != nil {
			t.Fatal(err)
		}
		return s
	}
	r := newClaim("running-owner")
	status, cells, err := invoke(opSessionNativeReconcile, r)
	if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "3" {
		t.Fatal("atomic interrupted source reconciliation", status, cells, err)
	}
	if token(r) != r.token("invalid") || count(`SELECT count(*) FROM session_native_recovery_fences WHERE session_id=$1`, r.Session) != 1 || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, r.Session) != 3 {
		t.Fatal("partial recovery state")
	}
	var terminal string
	if err := pool.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id=$1 AND sequence=3`, r.Session).Scan(&terminal); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(terminal, `"status":"cancelled"`) || strings.Contains(terminal, "verification_proofs") {
		t.Fatal("recovery invented completion or verification", terminal)
	}
	if count(`SELECT count(*) FROM learning_projection_outbox WHERE session_id=$1`, r.Session) != 1 {
		t.Fatal("terminal not transactionally enqueued")
	}
	status, retry, err := invoke(opSessionNativeReconcile, r)
	if err != nil || status != store.StatusOK || len(retry) != 1 || retry[0] != cells[0] || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, r.Session) != 3 {
		t.Fatal("duplicate recovery terminal", status, retry, err)
	}
	// Close and reopen a connection pool: the source write fence is durable.
	restarted, err := pgxpool.NewWithConfig(ctx, config.Copy())
	if err != nil {
		t.Fatal(err)
	}
	tx, err := restarted.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err = sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{r.Session, "late-write", r.Turn, "actor", rawMessage})
	tx.Rollback(ctx)
	restarted.Close()
	if err != nil || status != store.StatusInvalid {
		t.Fatal("restart lost recovered-turn writer fence", status, err)
	}
	for _, verified := range []bool{false, true} {
		status, _, err := appendEvent(r, "new-late-write", rawMessage, verified)
		if err != nil || status != store.StatusInvalid {
			t.Fatal("late ordinary/host writer passed fence", verified, status, err)
		}
	}
	status, replay, err := appendEvent(r, "1", rawMessage, false)
	if err != nil || status != store.StatusOK || replay[0] != "2" {
		t.Fatal("exact immutable retry lost", status, replay, err)
	}
	historyTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, history, err := sessionIRHistoryLoad(ctx, sessionIRLiveTx{historyTx}, []string{r.Session, "primary", "actor", ""})
	historyTx.Rollback(ctx)
	if err != nil || status != store.StatusOK || len(history) != 1 || !strings.Contains(history[0], "retained original") {
		t.Fatal("canonical history was altered", status, history, err)
	}
	for _, mutate := range []func(*nativeOwnerRequest){func(r *nativeOwnerRequest) { r.Owner = strings.Repeat("1", 32) }, func(r *nativeOwnerRequest) { r.Binding = strings.Repeat("2", 64) }, func(r *nativeOwnerRequest) { r.Containment = strings.Repeat("3", 64) }} {
		changed := r
		mutate(&changed)
		status, _, err := invoke(opSessionNativeReconcile, changed)
		if err != nil || status != store.StatusInvalid {
			t.Fatal("foreign resource identity admitted", status, err)
		}
	}
	for _, end := range []string{"completed", "failed", "cancelled"} {
		done := newClaim("terminal-" + end)
		raw := fmt.Sprintf(`{"kind":"execution","payload":{"status":%q}}`, end)
		status, _, err := appendEvent(done, "existing-terminal", raw, false)
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		done.Expected = done.token("finishing")
		if _, err := pool.Exec(ctx, `UPDATE session_harness_handles SET native_id=$3 WHERE session_id=$1 AND harness=$2`, done.Session, done.key(), done.Expected); err != nil {
			t.Fatal(err)
		}
		status, cells, err := invoke(opSessionNativeReconcile, done)
		if err != nil || status != store.StatusOK || cells[0] != "3" || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, done.Session) != 3 {
			t.Fatal("existing terminal changed or duplicated", status, cells, err)
		}
	}
	legacy := newClaim("legacy-no-custody")
	if _, err := pool.Exec(ctx, `DELETE FROM session_native_cache_owners WHERE session_id=$1`, legacy.Session); err != nil {
		t.Fatal(err)
	}
	status, _, err = invoke(opSessionNativeReconcile, legacy)
	if err != nil || status != store.StatusMissing || token(legacy) != legacy.Expected {
		t.Fatal("legacy pending claim was cleared", status, err)
	}
	stale := newClaim("replacement-cache")
	replacement := "pending:" + strings.Repeat("9", 32) + ":replacement-turn"
	if _, err := pool.Exec(ctx, `UPDATE session_harness_handles SET native_id=$3 WHERE session_id=$1 AND harness=$2`, stale.Session, stale.key(), replacement); err != nil {
		t.Fatal(err)
	}
	status, _, err = invoke(opSessionNativeReconcile, stale)
	if err != nil || status != store.StatusMissing || token(stale) != replacement {
		t.Fatal("stale recovery invalidated replacement", status, err)
	}
	// Real transactional faults after append/fence and after cache CAS roll back
	// the terminal, queue, source fence, cache token and owner row together.
	for n, at := range []string{"UPDATE session_harness_handles", "UPDATE session_native_cache_owners"} {
		fault := newClaim(fmt.Sprintf("rollback-%d", n))
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		_, _, err = sessionNativeReconcile(ctx, nativeRecoveryFault{sessionIRLiveTx{tx}, at}, nativeRecoveryFields(fault))
		tx.Rollback(ctx)
		if err == nil || token(fault) != fault.Expected || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, fault.Session) != 2 || count(`SELECT count(*) FROM session_native_recovery_fences WHERE session_id=$1`, fault.Session) != 0 || count(`SELECT count(*) FROM learning_projection_outbox WHERE session_id=$1`, fault.Session) != 0 {
			t.Fatal("recovery fault committed partial state", at, err)
		}
		status, _, err := invoke(opSessionNativeReconcile, fault)
		if err != nil || status != store.StatusOK {
			t.Fatal("restart after transaction rollback", status, err)
		}
	}
	concurrent := newClaim("serialized-old-writer")
	writer, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err = sessionIRAppend(ctx, sessionIRLiveTx{writer}, []string{concurrent.Session, "before-fence", concurrent.Turn, "actor", rawMessage})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	result := make(chan error, 1)
	go func() {
		status, _, err := invoke(opSessionNativeReconcile, concurrent)
		if err == nil && status != store.StatusOK {
			err = fmt.Errorf("reconcile status %d", status)
		}
		result <- err
	}()
	select {
	case err := <-result:
		t.Fatal("recovery passed live source row lock", err)
	case <-time.After(30 * time.Millisecond):
	}
	if err := writer.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	status, _, err = appendEvent(concurrent, "after-fence", rawMessage, false)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("old writer appended after recovery", status, err)
	}
	// New producer registration atomically replaces an invalid old cache owner;
	// a delayed reconciliation cannot invalidate that new request.
	next := r
	next.Turn = "replacement-turn"
	next.Owner = strings.Repeat("1", 32)
	next.Claim = strings.Repeat("2", 32)
	next.Expected = r.token("invalid")
	status, _, err = appendEvent(next, "new-owner-running", `{"kind":"execution","payload":{"status":"running"}}`, false)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	status, _, err = invoke(opSessionNativeOwnerClaim, next)
	if err != nil || status != store.StatusOK {
		t.Fatal("new qualified owner registration", status, err)
	}
	status, _, err = invoke(opSessionNativeReconcile, r)
	if err != nil || status != store.StatusInvalid || token(next) != next.token("pending") {
		t.Fatal("old recovery replaced a registered new owner", status, err)
	}

	// Partial source retention cannot remove the durable late-writer fence.
	lost := newClaim("deleted-terminal")
	status, _, err = invoke(opSessionNativeReconcile, lost)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id=$1 AND sequence=3`, lost.Session); err != nil {
		t.Fatal(err)
	}
	status, _, err = appendEvent(lost, "late-after-source-deletion", rawMessage, false)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("source retention removed writer fence", status, err)
	}
	status, _, err = invoke(opSessionNativeReconcile, lost)
	if status == store.StatusOK {
		t.Fatal("missing terminal was treated as live recovery receipt", err)
	}

	foreign := newClaim("foreign-source-actor")
	if _, err := pool.Exec(ctx, `UPDATE session_ir_events SET actor='foreign' WHERE session_id=$1 AND event_id='1'`, foreign.Session); err != nil {
		t.Fatal(err)
	}
	status, _, err = invoke(opSessionNativeReconcile, foreign)
	if err == nil || token(foreign) != foreign.Expected {
		t.Fatal("foreign observation authorized cancellation", status, err)
	}
	conflict := newClaim("conflicting-terminals")
	for _, id := range []string{"terminal-one", "terminal-two"} {
		status, _, err = appendEvent(conflict, id, runningDone, false)
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
	}
	status, _, err = invoke(opSessionNativeReconcile, conflict)
	if err == nil || token(conflict) != conflict.Expected {
		t.Fatal("conflicting terminal lifecycle accepted", status, err)
	}

	producer := newClaim("producer-atomic-registration")
	if _, err := pool.Exec(ctx, `DELETE FROM session_native_cache_owners WHERE session_id=$1`, producer.Session); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM session_harness_handles WHERE session_id=$1`, producer.Session); err != nil {
		t.Fatal(err)
	}
	producer.Expected = ""
	producerTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = sessionNativeOwnerClaim(ctx, nativeRecoveryFault{sessionIRLiveTx{producerTx}, "INSERT INTO session_native_cache_owners"}, nativeRecoveryFields(producer))
	producerTx.Rollback(ctx)
	if err == nil || count(`SELECT count(*) FROM session_harness_handles WHERE session_id=$1`, producer.Session) != 0 || count(`SELECT count(*) FROM session_native_cache_owners WHERE session_id=$1`, producer.Session) != 0 {
		t.Fatal("producer registration committed an unbound pending claim", err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM server_sessions WHERE id=$1`, r.Session); err != nil {
		t.Fatal(err)
	}
	if count(`SELECT count(*) FROM session_native_cache_owners WHERE session_id=$1`, r.Session) != 0 || count(`SELECT count(*) FROM session_native_recovery_fences WHERE session_id=$1`, r.Session) != 0 {
		t.Fatal("erasure retained recovery metadata")
	}
}

// Decimal wire strings must never select or order durable numeric cursors.
// These SQL fixtures reproduce a real resumed Claude turn's 3 -> 12 lifecycle;
// they establish store ordering, not that an SDK or configured tests executed.
func TestSessionNativeLiveNumericSourceOrder(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("native_numeric_order_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql"} {
		sql, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(sql)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('lifecycle','actor'),('external','actor')`); err != nil {
		t.Fatal(err)
	}
	for _, r := range []struct {
		session       string
		sequence      int
		turn, payload string
	}{
		{"lifecycle", 3, "turn", `{"kind":"execution","payload":{"status":"running"}}`},
		{"lifecycle", 12, "turn", `{"kind":"execution","payload":{"status":"completed"}}`},
		{"external", 9, "old", `{"kind":"history","payload":{"phase":"commit","scope":"external-sdk:claude-code"}}`},
		{"external", 10, "new", `{"kind":"history","payload":{"phase":"commit","scope":"external-sdk:claude-code"}}`},
	} {
		if _, err = pool.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES ($1,$2,$3,$4,'actor',$5)`, r.session, r.sequence, fmt.Sprintf("event-%d", r.sequence), r.turn, r.payload); err != nil {
			t.Fatal(err)
		}
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	state, sequence, _, err := nativeOwnerLifecycle(ctx, sessionIRLiveTx{tx}, "actor", nativeOwnerRequest{Session: "lifecycle", Turn: "turn"})
	if err != nil || state != "completed" || sequence != "12" {
		t.Fatal("3 must precede 12", state, sequence, err)
	}
	var turn, event, payload string
	if err = tx.QueryRow(ctx, sessionSearchExternalCommitSQL, "external", "actor").Scan(&sequence, &turn, &event, &payload); err != nil || sequence != "10" || turn != "new" {
		t.Fatal("latest is numeric 10, not lexical 9", sequence, turn, err)
	}
}
