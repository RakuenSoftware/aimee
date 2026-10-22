package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestSessionNativeLivePreexecReservationAndSharedEpoch(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	schema := fmt.Sprintf("native_reservation_%d", time.Now().UnixNano())
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
	run := func(op uint32, body any) (uint32, []string, error) {
		raw, _ := json.Marshal(body)
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		defer tx.Rollback(ctx)
		fn := sessionNativeReserve
		switch op {
		case opSessionNativeReservationReconcile:
			fn = sessionNativeReservationReconcile
		case opSessionNativeReservationFinish:
			fn = sessionNativeReservationFinish
		case opSessionNativeEpoch:
			fn = sessionNativeEpoch
		case opSessionNativeReservationLookup:
			fn = sessionNativeReservationLookup
		case opSessionNativeOwnerClaim:
			fn = sessionNativeOwnerClaim
		case opSessionNativeReconcile:
			fn = sessionNativeReconcile
		}
		status, cells, err := fn(ctx, sessionIRLiveTx{tx}, []string{"actor", string(raw)})
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, cells, err
	}
	count := func(sql string, args ...any) int {
		var n int
		if err := pool.QueryRow(ctx, sql, args...).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	appendRunning := func(r nativeReservationRequest) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{r.Session, "running:" + r.Turn, r.Turn, "actor", `{"kind":"execution","payload":{"status":"running"}}`})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if err := tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	minimal := func(r nativeReservationRequest) nativeReservationRequest {
		return nativeReservationRequest{Version: 1, Session: r.Session, Harness: r.Harness, Binding: r.Binding}
	}
	lookup := func(r nativeReservationRequest) nativeReservation {
		q := minimal(r)
		q.Owner = r.Owner
		status, cells, err := run(opSessionNativeReservationLookup, q)
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			t.Fatal(status, cells, err)
		}
		var v nativeReservation
		if err := json.Unmarshal([]byte(cells[0]), &v); err != nil {
			t.Fatal(err)
		}
		return v
	}
	makeRequest := func(name string) nativeReservationRequest {
		t.Helper()
		r := nativeReservationRequest{Version: 1, Session: name, Turn: "source-turn", Harness: "codex", Binding: strings.Repeat("b", 64), Descriptor: `{"version":1,"kind":"metadata-fixture-not-execution-proof"}`}
		sum := sha256.Sum256([]byte(name))
		r.Owner = hex.EncodeToString(sum[:16])
		sum = sha256.Sum256([]byte(r.Descriptor))
		r.Containment = hex.EncodeToString(sum[:])
		if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, name); err != nil {
			t.Fatal(err)
		}
		appendRunning(r)
		return r
	}
	t.Run("active session owners cross model and SDK boundaries", func(t *testing.T) {
		active := func(actor, session string) (uint32, []nativeReservation, error) {
			tx, err := pool.Begin(ctx)
			if err != nil {
				return 0, nil, err
			}
			defer tx.Rollback(ctx)
			status, cells, err := sessionNativeActiveOwned(ctx, sessionIRLiveTx{tx}, []string{actor, session})
			var owners []nativeReservation
			if err == nil && status == store.StatusOK {
				if len(cells) != 1 {
					return 0, nil, fmt.Errorf("missing whole owner list")
				}
				err = json.Unmarshal([]byte(cells[0]), &owners)
			}
			return status, owners, err
		}
		first := makeRequest("active-native-model-switch")
		status, owners, err := active("actor", first.Session)
		if err != nil || status != store.StatusOK || len(owners) != 0 {
			t.Fatal(status, owners, err)
		}
		if status, _, err := run(opSessionNativeReserve, first); err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		second := first
		second.Harness, second.Binding, second.Owner, second.Turn = "claude", strings.Repeat("a", 64), strings.Repeat("1", 32), "other-sdk-turn"
		appendRunning(second)
		if status, _, err := run(opSessionNativeReserve, second); err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		status, owners, err = active("actor", first.Session)
		if err != nil || status != store.StatusOK || len(owners) != 2 || owners[0].Owner != first.Owner || owners[1].Owner != second.Owner || owners[0].Descriptor != first.Descriptor {
			t.Fatal(status, owners, err)
		}
		if status, owners, err = active("foreign", first.Session); err != nil || status == store.StatusOK || len(owners) != 0 {
			t.Fatal("foreign owner disclosure", status, owners, err)
		}
		if _, err := pool.Exec(ctx, `UPDATE session_native_reservations SET actor='foreign' WHERE session_id=$1 AND owner_id=$2`, first.Session, second.Owner); err != nil {
			t.Fatal(err)
		}
		if status, owners, err = active("actor", first.Session); err != nil || status == store.StatusOK || len(owners) != 0 {
			t.Fatal("partial list concealed corrupt owner", status, owners, err)
		}
		if _, err := pool.Exec(ctx, `UPDATE session_native_reservations SET actor='actor' WHERE session_id=$1`, first.Session); err != nil {
			t.Fatal(err)
		}
		first.Epoch, first.Descriptor = "0", ""
		if status, _, err := run(opSessionNativeReservationReconcile, first); err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if status, owners, err = active("actor", first.Session); err != nil || status != store.StatusOK || len(owners) != 1 || owners[0].Owner != second.Owner {
			t.Fatal("reconciled owner remained active", status, owners, err)
		}
		if _, err := pool.Exec(ctx, `INSERT INTO session_native_reservations(session_id,harness,binding_sha256,owner_id,turn_id,actor,epoch,containment_sha256,containment_json) SELECT session_id,'codex',lpad(i::text,64,'b'),lpad(i::text,32,'c'),turn_id,actor,epoch,containment_sha256,containment_json FROM session_native_reservations CROSS JOIN generate_series(1,32) i WHERE session_id=$1 AND owner_id=$2`, second.Session, second.Owner); err != nil {
			t.Fatal(err)
		}
		if status, owners, err = active("actor", first.Session); err != nil || status != store.StatusInvalid || len(owners) != 0 {
			t.Fatal("oversized list silently truncated", status, owners, err)
		}
	})
	r := makeRequest("preexec-only")
	status, initial, err := run(opSessionNativeEpoch, minimal(r))
	if err != nil || status != store.StatusOK || len(initial) != 1 || initial[0] != "0" {
		t.Fatal("initial epoch broke existing cache namespace", status, initial, err)
	}
	status, cells, err := run(opSessionNativeReserve, r)
	if err != nil || status != store.StatusOK || cells[0] != "0" {
		t.Fatal(status, cells, err)
	}
	r.Epoch = cells[0]
	status, _, err = run(opSessionNativeReserve, r)
	if err != nil || status != store.StatusOK {
		t.Fatal("preexec retry", status, err)
	}
	if count(`SELECT count(*) FROM session_native_cache_owners WHERE session_id=$1`, r.Session) != 0 || count(`SELECT count(*) FROM session_harness_handles WHERE session_id=$1`, r.Session) != 0 {
		t.Fatal("preexec reservation invented a pending claim")
	}
	directoryStatus, directoryCells, directoryErr := run(opSessionNativeReservationLookup, minimal(r))
	if directoryErr != nil || directoryStatus != store.StatusOK || len(directoryCells) != 1 {
		t.Fatal("missing marker hid durable active owner", directoryStatus, directoryErr)
	}
	var directoryOwner nativeReservation
	if err := json.Unmarshal([]byte(directoryCells[0]), &directoryOwner); err != nil || directoryOwner.Owner != r.Owner {
		t.Fatal("directory lookup lost exact owner", directoryOwner, err)
	}
	v := lookup(r)
	if v.State != "active" || v.Descriptor != r.Descriptor || v.Claim != "" || v.Actor != "actor" {
		t.Fatal("reservation lost exact source descriptor", v)
	}
	changed := r
	changed.Owner = strings.Repeat("c", 32)
	status, _, err = run(opSessionNativeReserve, changed)
	if err != nil || status != store.StatusMissing {
		t.Fatal("parallel preexec owner admitted", status, err)
	}
	changed = r
	changed.Descriptor = `{"foreign":true}`
	status, _, err = run(opSessionNativeReserve, changed)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("descriptor mismatch admitted", status, err)
	}
	reconcile := r
	reconcile.Descriptor = ""
	status, recovered, err := run(opSessionNativeReservationReconcile, reconcile)
	if err != nil || status != store.StatusOK || len(recovered) != 2 || recovered[0] != "2" || !nativeHex(recovered[1], 32) {
		t.Fatal("preclaim recovery failed", status, recovered, err)
	}
	if count(`SELECT count(*) FROM session_harness_handles WHERE session_id=$1`, r.Session) != 0 || count(`SELECT count(*) FROM session_native_cache_owners WHERE session_id=$1`, r.Session) != 0 {
		t.Fatal("preclaim recovery manufactured a cache claim")
	}
	status, retry, err := run(opSessionNativeReservationReconcile, reconcile)
	if err != nil || status != store.StatusOK || fmt.Sprint(retry) != fmt.Sprint(recovered) || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, r.Session) != 2 {
		t.Fatal("commit-before-marker-unlink retry duplicated source", status, retry, err)
	}
	v = lookup(r)
	if v.State != "reconciled" || v.RecoveryEpoch != recovered[1] || v.Terminal != "2" {
		t.Fatal("restart lookup lost durable reconciliation", v)
	}
	status, epoch, err := run(opSessionNativeEpoch, minimal(r))
	if err != nil || status != store.StatusOK || epoch[0] != recovered[1] {
		t.Fatal("directory-wide epoch lost", status, epoch, err)
	}
	other := minimal(r)
	other.Binding = strings.Repeat("d", 64)
	status, epoch, err = run(opSessionNativeEpoch, other)
	if err != nil || status != store.StatusOK || epoch[0] != "0" {
		t.Fatal("epoch escaped binding scope", status, epoch, err)
	}

	// A later qualified producer must bind the exact reservation/epoch. Actual
	// post-config scope fields are fixture custody metadata, not execution proof.
	next := r
	next.Turn = "next-turn"
	next.Owner = strings.Repeat("e", 32)
	next.Epoch = recovered[1]
	appendRunning(next)
	status, _, err = run(opSessionNativeReserve, next)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	claim := nativeRecoveryFixture()
	claim.Session = next.Session
	claim.Turn = next.Turn
	claim.Binding = next.Binding
	claim.Owner = next.Owner
	claim.Containment = next.Containment
	claim.Epoch = next.Epoch
	status, _, err = run(opSessionNativeOwnerClaim, claim)
	if err != nil || status != store.StatusOK {
		t.Fatal("post-config owner was not bound to reservation", status, err)
	}
	v = lookup(next)
	if v.Expected != claim.token("pending") || v.Claim != claim.Claim || v.Scope != claim.Scope || v.Context != claim.Context || v.Epoch != next.Epoch {
		t.Fatal("post-config binding was not atomic", v)
	}
	claim.Expected = claim.token("pending")
	status, _, err = run(opSessionNativeReconcile, claim)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("qualified owner bypassed shared epoch reconciliation", status, err)
	}
	current := claim.token("finishing")
	if _, err := pool.Exec(ctx, `UPDATE session_harness_handles SET native_id=$3 WHERE session_id=$1 AND harness=$2`, claim.Session, claim.key(), current); err != nil {
		t.Fatal(err)
	}
	reconcile = next
	reconcile.Descriptor = ""
	reconcile.Expected = current
	status, recovered, err = run(opSessionNativeReservationReconcile, reconcile)
	if err != nil || status != store.StatusOK || recovered[1] == next.Epoch {
		t.Fatal("postclaim epoch did not advance", status, recovered, err)
	}
	var actual string
	if err := pool.QueryRow(ctx, `SELECT native_id FROM session_harness_handles WHERE session_id=$1 AND harness=$2`, claim.Session, claim.key()).Scan(&actual); err != nil {
		t.Fatal(err)
	}
	if actual != claim.token("invalid") {
		t.Fatal("exact pending/finishing claim was not invalidated", actual)
	}
	status, retry, err = run(opSessionNativeReservationReconcile, reconcile)
	if err != nil || status != store.StatusOK || fmt.Sprint(retry) != fmt.Sprint(recovered) {
		t.Fatal("postclaim crash-after-commit retry failed", status, retry, err)
	}
	wrong := reconcile
	wrong.Expected = claim.token("pending")
	status, _, err = run(opSessionNativeReservationReconcile, wrong)
	if err != nil || status != store.StatusMissing {
		t.Fatal("non-exact recovery token accepted", status, err)
	}

	// Failure after source cancellation or epoch CAS must roll everything back.
	for n, at := range []string{"UPDATE session_native_cache_epochs", "UPDATE session_native_reservations SET state"} {
		fault := makeRequest(fmt.Sprintf("epoch-rollback-%d", n))
		status, cells, err := run(opSessionNativeReserve, fault)
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		fault.Epoch = cells[0]
		fault.Descriptor = ""
		raw, _ := json.Marshal(fault)
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		_, _, err = sessionNativeReservationReconcile(ctx, nativeRecoveryFault{sessionIRLiveTx{tx}, at}, []string{"actor", string(raw)})
		tx.Rollback(ctx)
		if err == nil || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, fault.Session) != 1 || count(`SELECT count(*) FROM session_native_recovery_fences WHERE session_id=$1`, fault.Session) != 0 {
			t.Fatal("reservation recovery partially committed", at, err)
		}
		status, oldEpoch, err := run(opSessionNativeEpoch, minimal(fault))
		if err != nil || status != store.StatusOK || oldEpoch[0] != "0" {
			t.Fatal("failed transaction advanced epoch", status, oldEpoch, err)
		}
		status, _, err = run(opSessionNativeReservationReconcile, fault)
		if err != nil || status != store.StatusOK {
			t.Fatal("restart after rolled back recovery", status, err)
		}
	}
	// Normal terminal cleanup retires a reservation without invalidating a valid
	// cache namespace; an active running source cannot be declared finished.
	clean := makeRequest("normal-finish")
	status, cells, err = run(opSessionNativeReserve, clean)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	clean.Epoch = cells[0]
	finish := clean
	finish.Descriptor = ""
	status, _, err = run(opSessionNativeReservationFinish, finish)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("running source marked clean", status, err)
	}
	cleanClaim := nativeRecoveryFixture()
	cleanClaim.Session = clean.Session
	cleanClaim.Turn = clean.Turn
	cleanClaim.Binding = clean.Binding
	cleanClaim.Owner = clean.Owner
	cleanClaim.Containment = clean.Containment
	cleanClaim.Epoch = clean.Epoch
	cleanClaim.Claim = strings.Repeat("7", 32)
	status, _, err = run(opSessionNativeOwnerClaim, cleanClaim)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	end, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err = sessionIRAppend(ctx, sessionIRLiveTx{end}, []string{clean.Session, "completed", clean.Turn, "actor", runningDone})
	if err != nil || status != store.StatusOK {
		end.Rollback(ctx)
		t.Fatal(status, err)
	}
	if err := end.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	finish.Expected = cleanClaim.token("pending")
	status, _, err = run(opSessionNativeReservationFinish, finish)
	if err != nil || status != store.StatusMissing {
		t.Fatal("pending cache owner was declared clean", status, err)
	}
	finish.Expected = cleanClaim.token("complete")
	if _, err := pool.Exec(ctx, `UPDATE session_harness_handles SET native_id=$3 WHERE session_id=$1 AND harness=$2`, clean.Session, cleanClaim.key(), finish.Expected); err != nil {
		t.Fatal(err)
	}
	status, finished, err := run(opSessionNativeReservationFinish, finish)
	if err != nil || status != store.StatusOK || len(finished) != 3 || finished[1] != "0" || finished[2] != "finished" {
		t.Fatal("normal finish rotated existing cache namespace", status, finished, err)
	}
	status, finishedRetry, err := run(opSessionNativeReservationReconcile, finish)
	if err != nil || status != store.StatusOK || fmt.Sprint(finishedRetry) != fmt.Sprint(finished[:2]) {
		t.Fatal("restart after normal finish did not preserve cache", status, finishedRetry, err)
	}
	if count(`SELECT count(*) FROM session_native_recovery_fences WHERE session_id=$1`, clean.Session) != 0 {
		t.Fatal("ordinary finish fabricated recovery fence")
	}
	cleanNext := clean
	cleanNext.Owner = strings.Repeat("8", 32)
	cleanNext.Turn = "new-clean-turn"
	appendRunning(cleanNext)
	status, _, err = run(opSessionNativeReserve, cleanNext)
	if err != nil || status != store.StatusOK {
		t.Fatal("normal finish blocked subsequent successful turn", status, err)
	}
	missingSource := makeRequest("missing-source")
	status, _, err = run(opSessionNativeReserve, missingSource)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id=$1`, missingSource.Session); err != nil {
		t.Fatal(err)
	}
	status, _, err = run(opSessionNativeReservationLookup, minimal(missingSource))
	if err == nil {
		t.Fatal("directory lookup returned erased source authority", status)
	}
	// The lost-ack preclaim path may acknowledge definitive absence only after a
	// real terminal permanently prevents delayed reserve admission. No fake owner
	// row or terminal is created by this cleanup acknowledgment.
	absent := makeRequest("never-reserved")
	absent.Epoch = "0"
	finishAbsent := absent
	finishAbsent.Descriptor = ""
	status, _, err = run(opSessionNativeReservationFinish, finishAbsent)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("running source acknowledged absent", status, err)
	}
	terminalTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer terminalTx.Rollback(context.Background())
	status, _, err = sessionIRAppend(ctx, sessionIRLiveTx{terminalTx}, []string{absent.Session, "actual-failed-terminal", absent.Turn, "actor", `{"kind":"execution","payload":{"status":"failed"}}`})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	type delayedResult struct {
		status uint32
		err    error
	}
	delayed := make(chan delayedResult, 1)
	pidReady := make(chan uint32, 1)
	go func() {
		tx, e := pool.Begin(ctx)
		if e != nil {
			delayed <- delayedResult{0, e}
			return
		}
		defer tx.Rollback(ctx)
		pidReady <- tx.Conn().PgConn().PID()
		raw, _ := json.Marshal(absent)
		st, _, e := sessionNativeReserve(ctx, sessionIRLiveTx{tx}, []string{"actor", string(raw)})
		if e == nil && st == store.StatusOK {
			e = tx.Commit(ctx)
		}
		delayed <- delayedResult{st, e}
	}()
	pid := <-pidReady
	deadline := time.Now().Add(time.Second)
	blocked := false
	for time.Now().Before(deadline) {
		if e := pool.QueryRow(ctx, `SELECT coalesce(wait_event_type='Lock',false) FROM pg_stat_activity WHERE pid=$1`, pid).Scan(&blocked); e != nil {
			t.Fatal(e)
		}
		if blocked {
			break
		}
		time.Sleep(time.Millisecond)
	}
	if !blocked {
		t.Fatal("delayed reserve did not reach the actual source lock")
	}
	raw, _ := json.Marshal(finishAbsent)
	status, absence, err := sessionNativeReservationFinish(ctx, sessionIRLiveTx{terminalTx}, []string{"actor", string(raw)})
	if err != nil || status != store.StatusOK || len(absence) != 3 || absence[2] != "absent" || absence[1] != "0" {
		t.Fatal("absence cleanup lost explicit disposition", status, absence, err)
	}
	if err := terminalTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	late := <-delayed
	if late.err != nil || late.status != store.StatusInvalid {
		t.Fatal("delayed reserve reopened terminal source", late)
	}
	if count(`SELECT count(*) FROM session_native_reservations WHERE session_id=$1`, absent.Session) != 0 || count(`SELECT count(*) FROM session_native_cache_owners WHERE session_id=$1`, absent.Session) != 0 || count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, absent.Session) != 2 {
		t.Fatal("absence cleanup invented durable execution/owner evidence")
	}
	status, retryAbsence, err := run(opSessionNativeReservationFinish, finishAbsent)
	if err != nil || status != store.StatusOK || fmt.Sprint(retryAbsence) != fmt.Sprint(absence) {
		t.Fatal("absence ack restart changed", status, retryAbsence, err)
	}
	changedAbsent := finishAbsent
	changedAbsent.Epoch = strings.Repeat("e", 32)
	status, _, err = run(opSessionNativeReservationFinish, changedAbsent)
	if err != nil || status != store.StatusMissing {
		t.Fatal("stale absence epoch acknowledged", status, err)
	}
	changedAbsent = finishAbsent
	changedAbsent.Expected = "pending:not-a-claim"
	status, _, err = run(opSessionNativeReservationFinish, changedAbsent)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("absence ack accepted claimed phase", status, err)
	}
	// Rollback of an unpublished cleanup transaction cannot fence the still
	// running source or fabricate any committed absence receipt.
	rolled := makeRequest("absence-rollback")
	rolled.Epoch = "0"
	rb, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, _, err = sessionIRAppend(ctx, sessionIRLiveTx{rb}, []string{rolled.Session, "rolled-terminal", rolled.Turn, "actor", `{"kind":"execution","payload":{"status":"failed"}}`})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	rolledFinish := rolled
	rolledFinish.Descriptor = ""
	raw, _ = json.Marshal(rolledFinish)
	status, _, err = sessionNativeReservationFinish(ctx, sessionIRLiveTx{rb}, []string{"actor", string(raw)})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if err := rb.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if count(`SELECT count(*) FROM session_ir_events WHERE session_id=$1`, rolled.Session) != 1 || count(`SELECT count(*) FROM session_native_reservations WHERE session_id=$1`, rolled.Session) != 0 {
		t.Fatal("rollback retained cleanup evidence")
	}
	status, _, err = run(opSessionNativeReserve, rolled)
	if err != nil || status != store.StatusOK {
		t.Fatal("uncommitted absence acknowledged externally", status, err)
	}
	// A forged persisted descriptor is never returned as a valid owner receipt.
	corrupt := makeRequest("corrupt-descriptor")
	status, _, err = run(opSessionNativeReserve, corrupt)
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if _, err := pool.Exec(ctx, `UPDATE session_native_reservations SET containment_json='{"changed":true}' WHERE session_id=$1`, corrupt.Session); err != nil {
		t.Fatal(err)
	}
	q := minimal(corrupt)
	q.Owner = corrupt.Owner
	status, _, err = run(opSessionNativeReservationLookup, q)
	if err == nil {
		t.Fatal("corrupt containment descriptor admitted", status)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM server_sessions WHERE id=$1`, r.Session); err != nil {
		t.Fatal(err)
	}
	if count(`SELECT count(*) FROM session_native_reservations WHERE session_id=$1`, r.Session) != 0 || count(`SELECT count(*) FROM session_native_cache_epochs WHERE session_id=$1`, r.Session) != 0 {
		t.Fatal("erasure retained reservation metadata")
	}
}
