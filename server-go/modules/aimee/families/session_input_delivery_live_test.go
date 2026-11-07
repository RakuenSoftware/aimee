// Owned-runtime overlay only: real PostgreSQL + unchanged production Family.Handler.
package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

type ownedInputDB struct{ p *pgxpool.Pool }

func (d ownedInputDB) Exec(c context.Context, s string, a ...any) (store.Tag, error) {
	x, e := d.p.Exec(c, s, a...)
	return store.RowsAffected(x.RowsAffected()), ownedInputError(e)
}
func (d ownedInputDB) Query(c context.Context, s string, a ...any) (store.Rows, error) {
	x, e := d.p.Query(c, s, a...)
	if e != nil {
		return nil, e
	}
	return liveRows{x}, nil
}
func (d ownedInputDB) QueryRow(c context.Context, s string, a ...any) store.Row {
	return sessionIRLiveRow{d.p.QueryRow(c, s, a...)}
}
func (d ownedInputDB) Begin(c context.Context) (store.Tx, error) {
	x, e := d.p.Begin(c)
	if e != nil {
		return nil, e
	}
	return ownedDeliveryTx{sessionIRLiveTx{x}}, nil
}

// Mirror the production postgres SQL wire's typed refusal; do not classify
// driver text in the generic family.
func ownedInputError(err error) error {
	var pg *pgconn.PgError
	if errors.As(err, &pg) {
		return &store.StoreError{SQLState: pg.Code, Message: pg.Message, Op: "owned_sql"}
	}
	return err
}

type ownedDeliveryTx struct{ sessionIRLiveTx }

func (q ownedDeliveryTx) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	tag, err := q.sessionIRLiveTx.Exec(ctx, sql, args...)
	return tag, ownedInputError(err)
}

const ownedInputSchema = "owned_original_input"
const ownedInputUUID = "12345678-1234-4234-8234-123456789abc"

func ownedInputPool(t *testing.T) *pgxpool.Pool {
	t.Helper()
	cfg, e := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if e != nil {
		t.Fatal(e)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = ownedInputSchema
	p, e := pgxpool.NewWithConfig(context.Background(), cfg)
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(p.Close)
	return p
}
func ownedInputBatch(uuid, text, opaque string) string {
	h := sha256.Sum256([]byte(text))
	v := map[string]any{"schema_version": 1, "begin": map[string]any{"kind": "execution", "payload": map[string]any{"status": "running"}}, "input": map[string]any{"kind": "message", "message_id": "user", "payload": map[string]any{"role": "user", "blocks": []any{map[string]any{"type": "text", "text": text, "input_attachment": map[string]any{"schema_version": 1, "kind": "host.original-input.v1", "operation_uuid": uuid, "text_sha256": hex.EncodeToString(h[:]), "opaque": opaque}}}}}}
	b, _ := json.Marshal(v)
	return string(b)
}
func ownedInputCall(db ownedInputDB, op uint32, args ...string) (uint32, []string, error) {
	f, e := wire.EncodeFields(op, args)
	if e != nil {
		return 0, nil, e
	}
	b, s := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: 0}, f)
	if s != bus.ModuleStatusOK {
		return 0, nil, fmt.Errorf("module status %d", s)
	}
	status, cells, ok := store.DecodeRequest(b)
	if !ok {
		return 0, nil, fmt.Errorf("invalid response")
	}
	return status, cells, nil
}
func ownedInputCount(t *testing.T, p *pgxpool.Pool, table, sid string) int {
	t.Helper()
	var n int
	if e := p.QueryRow(context.Background(), "SELECT count(*) FROM "+table+" WHERE session_id=$1", sid).Scan(&n); e != nil {
		t.Fatal(e)
	}
	return n
}
func ownedInputExpect(t *testing.T, db ownedInputDB, want uint32, op uint32, a ...string) []string {
	t.Helper()
	s, c, e := ownedInputCall(db, op, a...)
	if e != nil || s != want {
		t.Fatalf("op %d want %d got %d err=%v", op, want, s, e)
	}
	return c
}

func deliveryFixture(t *testing.T) (ownedInputDB, *pgxpool.Pool) {
	t.Helper()
	if os.Getenv("OWNED_INPUT_DELIVERY_FIXTURE") != "1" || os.Getenv("AIMEE_TEST_PG_URL") == "" {
		t.Skip("owned PostgreSQL required")
	}
	p := ownedInputPool(t)
	return ownedInputDB{p}, p
}
func deliveryIntent(key, opaque string) string {
	b, _ := json.Marshal(inputDeliveryIntent{1, "source-owner", key, opaque})
	return string(b)
}
func deliveryRecord(t *testing.T, db ownedInputDB, sid, op string) inputDeliveryRecord {
	t.Helper()
	c := ownedInputExpect(t, db, store.StatusOK, 59, sid, op, "alice", "source-owner")
	var r inputDeliveryRecord
	if len(c) != 1 || json.Unmarshal([]byte(c[0]), &r) != nil {
		t.Fatal("bad delivery record")
	}
	return r
}
func TestOwnedDeliveryTransactions(t *testing.T) {
	db, p := deliveryFixture(t)
	ctx := context.Background()
	for _, sid := range []string{"delivery", "legacy-delivery", "delivery-race", "delivery-unknown"} {
		if _, e := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'alice')`, sid); e != nil {
			t.Fatal(e)
		}
	}
	key := deliveryHash("stable synthetic intent")
	batch := ownedInputBatch(ownedInputUUID, "safe original input", " {\"wide\":9007199254740993}\n")
	c := ownedInputExpect(t, db, store.StatusOK, 58, "delivery", ownedInputUUID, "turn-first", "alice", batch, deliveryIntent(key, "signed-first"))
	if len(c) != 6 || c[4] != "created" {
		t.Fatal("created receipt missing")
	}
	r := deliveryRecord(t, db, "delivery", ownedInputUUID)
	if r.Intent != "signed-first" || r.Turn != "turn-first" || r.Outcome != "pending" || ownedInputCount(t, p, "session_ir_events", "delivery") != 2 {
		t.Fatal("atomic pair/intent missing")
	}
	replay := ownedInputExpect(t, db, store.StatusOK, 58, "delivery", ownedInputUUID, "turn-retry", "alice", batch, deliveryIntent(key, "signed-retry"))
	if replay[4] != "replay" || replay[2] != "turn-first" || replay[5] != r.ID || deliveryRecord(t, db, "delivery", ownedInputUUID).Intent != "signed-first" {
		t.Fatal("replay changed first intent/turn")
	}
	ownedInputExpect(t, db, store.StatusInvalid, 58, "delivery", ownedInputUUID, "turn-retry", "alice", batch, deliveryIntent(deliveryHash("changed"), "signed-retry"))
	ownedInputExpect(t, db, store.StatusInvalid, 58, "delivery", ownedInputUUID, "turn-retry", "alice", ownedInputBatch(ownedInputUUID, "different safe text", "x"), deliveryIntent(key, "signed-first"))
	ownedInputExpect(t, db, store.StatusMissing, 59, "delivery", ownedInputUUID, "bob", "source-owner")
	ownedInputExpect(t, db, store.StatusMissing, 59, "delivery", ownedInputUUID, "alice", "different-receiver")
	// Historical ordinary56 cannot acquire a prepared intent retrospectively.
	ownedInputExpect(t, db, store.StatusOK, 56, "legacy-delivery", ownedInputUUID, "legacy-turn", "alice", batch)
	ownedInputExpect(t, db, store.StatusInvalid, 58, "legacy-delivery", ownedInputUUID, "retry", "alice", batch, deliveryIntent(key, "signed-first"))
	if ownedInputCount(t, p, "session_input_deliveries", "legacy-delivery") != 0 || ownedInputCount(t, p, "session_ir_events", "legacy-delivery") != 2 {
		t.Fatal("legacy retrofit/partial event")
	}
	// A response lost after durable commit leaves the exact original pending row.
	page := ownedInputExpect(t, db, store.StatusOK, 60, "source-owner", "alice", "0", "32", "4194304")
	var pending struct {
		Rows []inputDeliveryRecord `json:"rows"`
		Next string                `json:"next_cursor"`
	}
	if json.Unmarshal([]byte(page[0]), &pending) != nil || len(pending.Rows) != 1 || pending.Rows[0].ID != r.ID {
		t.Fatal("pending row not recoverable")
	}
	// Ack authorization and exact opaque result idempotency, including wide integers.
	result := `{"receipt":9007199254740993}`
	ownedInputExpect(t, db, store.StatusInvalid, 61, r.ID, "source-owner", "bob", key, "delivered", result)
	ownedInputExpect(t, db, store.StatusInvalid, 61, r.ID, "different-receiver", "alice", key, "delivered", result)
	ownedInputExpect(t, db, store.StatusOK, 61, r.ID, "source-owner", "alice", key, "delivered", result)
	ownedInputExpect(t, db, store.StatusOK, 61, r.ID, "source-owner", "alice", key, "delivered", result)
	ownedInputExpect(t, db, store.StatusInvalid, 61, r.ID, "source-owner", "alice", key, "delivered", `{"receipt":1}`)
	if got := deliveryRecord(t, db, "delivery", ownedInputUUID); got.Outcome != "delivered" || got.Result != result {
		t.Fatal("ack lost exact result")
	}
	page = ownedInputExpect(t, db, store.StatusOK, 60, "source-owner", "alice", "0", "32", "4194304")
	if json.Unmarshal([]byte(page[0]), &pending) != nil || len(pending.Rows) != 0 {
		t.Fatal("completed row remained pending")
	}
	// Rejected transaction cannot leave half a BEGIN/User or delivery record.
	occupied := ownedInputExpect(t, db, store.StatusOK, 58, "delivery-race", ownedInputUUID, "one-turn", "alice", batch, deliveryIntent(key, "signed-first"))
	if len(occupied) != 6 {
		t.Fatal("receipt")
	}
	other := "12345678-1234-4234-8234-123456789abd"
	ownedInputExpect(t, db, store.StatusInvalid, 58, "delivery-race", other, "one-turn", "alice", ownedInputBatch(other, "safe second input", "opaque"), deliveryIntent(deliveryHash("second"), "signed-second"))
	if ownedInputCount(t, p, "session_ir_events", "delivery-race") != 2 || ownedInputCount(t, p, "session_input_deliveries", "delivery-race") != 1 {
		t.Fatal("occupied turn partial append")
	}
	t.Log("PASS atomic pair+delivery, first-intent replay, changed intent/text refusal, owner/receiver authorization, no legacy retrofit, pending recovery, ack loss/idempotency, wide result bytes, occupied-turn rollback")
}
func TestOwnedDeliveryReopen(t *testing.T) {
	db, p := deliveryFixture(t)
	r := deliveryRecord(t, db, "delivery", ownedInputUUID)
	if r.Outcome != "delivered" || r.Intent != "signed-first" || r.Result != `{"receipt":9007199254740993}` || ownedInputCount(t, p, "session_ir_events", "delivery") != 2 {
		t.Fatal("reopen changed result")
	}
	t.Log("PASS new process/reopened PostgreSQL retains exact first intent, pair and documentary result")
}

func TestOwnedDeliveryKillAfterCommit(t *testing.T) {
	db, p := deliveryFixture(t)
	if _, e := p.Exec(context.Background(), `INSERT INTO server_sessions(id,principal) VALUES('delivery-kill','alice')`); e != nil {
		t.Fatal(e)
	}
	ownedInputExpect(t, db, store.StatusOK, 58, "delivery-kill", ownedInputUUID, "kill-turn", "alice", ownedInputBatch(ownedInputUUID, "safe killed input", "opaque"), deliveryIntent(deliveryHash("killed intent"), "prepared-before-commit"))
	// No Native or provider is linked into this process. Kill after handler commit.
	if e := syscall.Kill(os.Getpid(), syscall.SIGKILL); e != nil {
		t.Fatal(e)
	}
}
func TestOwnedDeliveryKillRecovery(t *testing.T) {
	db, p := deliveryFixture(t)
	r := deliveryRecord(t, db, "delivery-kill", ownedInputUUID)
	if r.Outcome != "pending" || r.Intent != "prepared-before-commit" || ownedInputCount(t, p, "session_ir_events", "delivery-kill") != 2 {
		t.Fatal("killed commit not recoverable")
	}
	c := ownedInputExpect(t, db, store.StatusOK, 58, "delivery-kill", ownedInputUUID, "new-caller-turn", "alice", ownedInputBatch(ownedInputUUID, "safe killed input", "opaque"), deliveryIntent(deliveryHash("killed intent"), "different-signature"))
	if c[4] != "replay" || c[2] != "kill-turn" || c[5] != r.ID {
		t.Fatal("kill replay not exact")
	}
	t.Log("PASS real SIGKILL after atomic canonical commit before any Native call: pending exact pair and prepared intent recovered; replay has no created outcome")
}

func TestOwnedDeliveryServer(t *testing.T) {
	db, _ := deliveryFixture(t)
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer cancel()
	handler := Sessions.Handler(db)
	wrapped := func(i bus.ModuleInvocation, f []byte) ([]byte, bus.ModuleStatus) {
		b, s := handler(i, f)
		op, a, ok := store.DecodeRequest(f)
		if os.Getenv("OWNED_INPUT_DROP_AFTER_COMMIT") == "1" && ok && op == 58 && len(a) == 6 && a[0] == "broker-delivery-lost" && s == bus.ModuleStatusOK {
			status, _, valid := store.DecodeRequest(b)
			if valid && status == store.StatusOK {
				os.Exit(0)
			}
		}
		return b, s
	}
	if e := bus.RunModuleProcess(ctx, bus.ModuleProcessConfig{SocketPath: os.Getenv("OWNED_INPUT_BUS_SOCKET"), ModuleName: "owned-input-delivery", PrincipalClass: 1, PrincipalRef: 30, Stages: []bus.ModuleStage{{EventKind: 11782, StageID: StageSessions}}, Handler: wrapped, MaxIdlePollInterval: time.Millisecond}); e != nil {
		t.Fatal(e)
	}
}
func TestOwnedDeliveryBrokerSetup(t *testing.T) {
	_, p := deliveryFixture(t)
	_, e := p.Exec(context.Background(), `INSERT INTO server_sessions(id,principal) VALUES('broker-delivery','alice'),('broker-delivery-lost','alice')`)
	if e != nil {
		t.Fatal(e)
	}
}

func TestOwnedDeliveryRetention(t *testing.T) {
	db, p := deliveryFixture(t)
	ctx := context.Background()
	key := deliveryHash("retention intent")
	intent := deliveryIntent(key, "retention-prepared")
	batch := ownedInputBatch(ownedInputUUID, "safe retained input", "opaque")
	for _, sid := range []string{"ret-pending", "ret-done", "ret-legacy", "ret-exp-pending", "ret-exp-done", "ret-sub-pending", "ret-sub-done"} {
		owner := "retention-alice"
		if sid == "ret-sub-pending" || sid == "ret-sub-done" {
			owner = "retention-subject"
		}
		if _, e := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,$2)`, sid, owner); e != nil {
			t.Fatal(e)
		}
		if sid == "ret-legacy" {
			ownedInputExpect(t, db, store.StatusOK, 56, sid, ownedInputUUID, "turn", owner, batch)
			continue
		}
		c := ownedInputExpect(t, db, store.StatusOK, 58, sid, ownedInputUUID, "turn", owner, batch, intent)
		if sid == "ret-done" || sid == "ret-exp-done" || sid == "ret-sub-done" {
			ownedInputExpect(t, db, store.StatusOK, 61, c[5], "source-owner", owner, key, "delivered", `{"known_fixture_result":true}`)
		}
	}
	ownedInputExpect(t, db, store.StatusConflict, opServerSessionDelete, "ret-pending")
	if ownedInputCount(t, p, "session_ir_events", "ret-pending") != 2 || ownedInputCount(t, p, "session_input_deliveries", "ret-pending") != 1 {
		t.Fatal("pending delete lost obligation")
	}
	// Trigger closes raw SQL/FK cascade too, not only the API classification.
	if _, e := p.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id='ret-pending'`); e == nil {
		t.Fatal("direct canonical cascade erased pending intent")
	}
	if ownedInputCount(t, p, "session_ir_events", "ret-pending") != 2 {
		t.Fatal("pending cascade partially deleted")
	}
	ownedInputExpect(t, db, store.StatusOK, opServerSessionDelete, "ret-done")
	ownedInputExpect(t, db, store.StatusOK, opServerSessionDelete, "ret-legacy")
	if ownedInputCount(t, p, "session_input_deliveries", "ret-done") != 0 {
		t.Fatal("resolved retention blocked normal deletion")
	}
	if _, e := p.Exec(ctx, `UPDATE server_sessions SET created_at=now()-interval '365 days' WHERE id IN ('ret-exp-pending','ret-exp-done')`); e != nil {
		t.Fatal(e)
	}
	c := ownedInputExpect(t, db, store.StatusOK, opServerSessionDeleteExpired, "86400")
	if len(c) != 1 || c[0] != "1" {
		t.Fatal("expired cleanup did not truthfully skip pending", c)
	}
	if ownedInputCount(t, p, "session_input_deliveries", "ret-exp-pending") != 1 || ownedInputCount(t, p, "session_input_deliveries", "ret-exp-done") != 0 {
		t.Fatal("expired pending/resolved retention mismatch")
	}
	// Subject deletion is all-or-nothing, including already-resolved siblings.
	ownedInputExpect(t, db, store.StatusConflict, opServerSessionEraseSubject, "delivery-subject-request-01", "retention-subject")
	if ownedInputCount(t, p, "session_ir_events", "ret-sub-pending") != 2 || ownedInputCount(t, p, "session_ir_events", "ret-sub-done") != 2 {
		t.Fatal("subject conflict partially deleted")
	}
	got := ownedInputExpect(t, db, store.StatusOK, 59, "ret-sub-pending", ownedInputUUID, "retention-subject", "source-owner")
	var r inputDeliveryRecord
	if json.Unmarshal([]byte(got[0]), &r) != nil {
		t.Fatal("record")
	}
	// Synthetic known terminal fixture; private authenticated Native result is a separate join.
	ownedInputExpect(t, db, store.StatusOK, 61, r.ID, "source-owner", "retention-subject", key, "terminal", `{"known_fixture_terminal":true}`)
	count := ownedInputExpect(t, db, store.StatusOK, opServerSessionEraseSubject, "delivery-subject-request-01", "retention-subject")
	if len(count) != 1 || count[0] != "2" {
		t.Fatal("resolved subject delete", count)
	}
	t.Log("PASS pending deletion explicit conflict/no loss; direct FK guard; resolved/legacy deletion; expired skip/count; subject rollback then known-fixture terminal permits normal cleanup")
}

func TestOwnedDeliveryRaces(t *testing.T) {
	db, p := deliveryFixture(t)
	ctx := context.Background()
	key := deliveryHash("race intent")
	batch := ownedInputBatch(ownedInputUUID, "safe raced input", "opaque")
	for _, sid := range []string{"race-delivery-same", "race-delivery-turn", "race-delivery-delete", "race-order-low", "race-order-high"} {
		if _, e := p.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'alice')`, sid); e != nil {
			t.Fatal(e)
		}
	}
	var wg sync.WaitGroup
	statuses := make([]uint32, 8)
	receipts := make([][]string, 8)
	errs := make([]error, 8)
	for i := range statuses {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			statuses[i], receipts[i], errs[i] = ownedInputCall(db, 58, "race-delivery-same", ownedInputUUID, fmt.Sprintf("proposed-%d", i), "alice", batch, deliveryIntent(key, fmt.Sprintf("signature-%d", i)))
		}(i)
	}
	wg.Wait()
	created := 0
	var first []string
	for i, s := range statuses {
		if errs[i] != nil || s != store.StatusOK || len(receipts[i]) != 6 {
			t.Fatal("sameUUID race", i, s, errs[i])
		}
		if receipts[i][4] == "created" {
			created++
			first = receipts[i]
		}
	}
	if created != 1 {
		t.Fatal("sameUUID created count", created)
	}
	for _, r := range receipts {
		if r[0] != first[0] || r[1] != first[1] || r[2] != first[2] || r[5] != first[5] {
			t.Fatal("sameUUID race changed original tuple")
		}
	}
	if ownedInputCount(t, p, "session_ir_events", "race-delivery-same") != 2 || ownedInputCount(t, p, "session_input_deliveries", "race-delivery-same") != 1 {
		t.Fatal("duplicate sameUUID")
	}
	for i := range statuses {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			op := fmt.Sprintf("12345678-1234-4234-8234-%012d", i+1)
			statuses[i], receipts[i], errs[i] = ownedInputCall(db, 58, "race-delivery-turn", op, "one-turn", "alice", ownedInputBatch(op, "safe raced input", "opaque"), deliveryIntent(deliveryHash(op), "signed"))
		}(i)
	}
	wg.Wait()
	created = 0
	for i, s := range statuses {
		if errs[i] != nil {
			t.Fatal(errs[i])
		}
		if s == store.StatusOK {
			created++
		} else if s != store.StatusInvalid {
			t.Fatal("turn race status", s)
		}
	}
	if created != 1 || ownedInputCount(t, p, "session_ir_events", "race-delivery-turn") != 2 || ownedInputCount(t, p, "session_input_deliveries", "race-delivery-turn") != 1 {
		t.Fatal("turn race partial/duplicate")
	}
	r := ownedInputExpect(t, db, store.StatusOK, 58, "race-delivery-delete", ownedInputUUID, "turn", "alice", batch, deliveryIntent(key, "signed"))
	wg.Add(2)
	var delStatus, ackStatus uint32
	var delErr, ackErr error
	go func() {
		defer wg.Done()
		delStatus, _, delErr = ownedInputCall(db, opServerSessionDelete, "race-delivery-delete")
	}()
	go func() {
		defer wg.Done()
		ackStatus, _, ackErr = ownedInputCall(db, 61, r[5], "source-owner", "alice", key, "delivered", `{"known_fixture_result":true}`)
	}()
	wg.Wait()
	if delErr != nil || ackErr != nil || ackStatus != store.StatusOK || (delStatus != store.StatusOK && delStatus != store.StatusConflict) {
		t.Fatal("ack/delete race", delStatus, ackStatus, delErr, ackErr)
	}
	ownedInputExpect(t, db, store.StatusOK, opServerSessionDelete, "race-delivery-delete")
	// BIGSERIAL is not a commit watermark. A full recovery pass must restart0.
	tx, e := p.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	defer tx.Rollback(ctx)
	status, low, e := sessionInputDeliveryBegin(ctx, sessionIRLiveTx{tx}, []string{"race-order-low", ownedInputUUID, "low-turn", "alice", batch, deliveryIntent(key, "low-first")})
	if e != nil || status != store.StatusOK {
		t.Fatal("low uncommitted", e, status)
	}
	high := ownedInputExpect(t, db, store.StatusOK, 58, "race-order-high", ownedInputUUID, "high-turn", "alice", batch, deliveryIntent(key, "high-first"))
	if e = tx.Commit(ctx); e != nil {
		t.Fatal(e)
	}
	lowID, _ := strconv.ParseInt(low[5], 10, 64)
	highID, _ := strconv.ParseInt(high[5], 10, 64)
	if lowID >= highID {
		t.Fatal("sequence order")
	}
	page := ownedInputExpect(t, db, store.StatusOK, 60, "source-owner", "alice", high[5], "32", "4194304")
	if strings.Contains(page[0], "low-first") {
		t.Fatal("cursor unexpectedly rewound")
	}
	page = ownedInputExpect(t, db, store.StatusOK, 60, "source-owner", "alice", "0", "32", "4194304")
	if !strings.Contains(page[0], "low-first") || !strings.Contains(page[0], "high-first") {
		t.Fatal("restart0 lost late commit")
	}
	t.Log("PASS concurrent sameUUID and occupied-turn invariants; ACK/delete lock order/no loss; out-of-order commit requires explicit reset0 recovery pass")
}
