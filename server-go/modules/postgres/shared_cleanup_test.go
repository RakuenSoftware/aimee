package postgres

import (
	"context"
	"encoding/binary"
	"errors"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db"
	domain "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Cancellation between statements does not cancel a PostgreSQL statement, so
// only an acknowledged rollback can release this real transaction and lock.
func TestCancelledFamilyReleasesStoreTransactionBeforeIdleTTL(t *testing.T) {
	pool := sharedTestDatabase(t)
	handler := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	store, err := db.NewStore(sharedStoreCaller{handler.handle})
	if err != nil {
		t.Fatal(err)
	}
	// Always drain a reproduced leak before the fixture pool/database cleanup.
	defer func() {
		handler.txs.mu.Lock()
		defer handler.txs.mu.Unlock()
		for id, transaction := range handler.txs.open {
			cleanup, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_ = transaction.tx.Rollback(cleanup)
			cancel()
			transaction.conn.Release()
			delete(handler.txs.open, id)
		}
	}()
	key := time.Now().UnixNano()
	acquired := false
	family := domain.Family{Name: "cancelled_cleanup_fixture", Stage: 1, Ops: map[uint32]domain.Op{1: {
		Name: "hold_until_cancelled", Args: 0, Tx: true,
		Run: func(ctx context.Context, q domain.Queryer, _ []string) (uint32, []string, error) {
			if _, err := q.Exec(ctx, "SELECT pg_advisory_xact_lock($1)", key); err != nil {
				return 0, nil, err
			}
			acquired = true
			<-ctx.Done()
			return 0, nil, ctx.Err()
		},
	}}}
	request := make([]byte, 8)
	binary.LittleEndian.PutUint32(request, 1)
	reply, transport := family.Handler(store)(bus.ModuleInvocation{StageID: 1}, request)
	if transport != bus.ModuleStatusOK || !acquired || len(reply) < 4 ||
		binary.LittleEndian.Uint32(reply) != domain.StatusFailed {
		t.Fatal("operation cancellation outcome changed", transport, acquired, reply)
	}
	handler.txs.mu.Lock()
	live := len(handler.txs.open)
	handler.txs.mu.Unlock()
	probe, err := pool.Begin(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer probe.Rollback(context.Background())
	var available bool
	if err = probe.QueryRow(context.Background(), "SELECT pg_try_advisory_xact_lock($1)", key).Scan(&available); err != nil {
		t.Fatal(err)
	}
	var idle int
	if err = pool.QueryRow(context.Background(), "SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND state='idle in transaction'").Scan(&idle); err != nil {
		t.Fatal(err)
	}
	if live != 0 || !available {
		t.Fatalf("cancelled operation retained transaction before TTL: registered=%d, independent_lock_available=%v, idle_in_transaction=%d", live, available, idle)
	}
}

type commitCleanupCaller struct {
	sharedStoreCaller
	commits, rollbacks int
	lostCommitAck      bool
}

func (c *commitCleanupCaller) Call(ctx context.Context, kind, stage uint32, id uint64, deadline time.Duration, request []byte) ([]byte, error) {
	switch binary.LittleEndian.Uint32(request) {
	case opCommit:
		c.commits++
	case opRollback:
		c.rollbacks++
	}
	reply, err := c.sharedStoreCaller.Call(ctx, kind, stage, id, deadline, request)
	if c.lostCommitAck && binary.LittleEndian.Uint32(request) == opCommit && err == nil {
		return nil, errors.New("commit acknowledgment unavailable")
	}
	return reply, err
}

// A successful body can cross its deadline after its last query. Commit must
// retain the error, while bounded same-handle rollback releases the real owner.
func TestCancelledBeforeCommitReleasesExactStoreTransaction(t *testing.T) {
	pool := sharedTestDatabase(t)
	if _, err := pool.Exec(context.Background(), `CREATE TABLE commit_cleanup_owner(id integer PRIMARY KEY);INSERT INTO commit_cleanup_owner VALUES(1)`); err != nil {
		t.Fatal(err)
	}
	handler := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	caller := &commitCleanupCaller{sharedStoreCaller: sharedStoreCaller{handler.handle}}
	store, err := db.NewStore(caller)
	if err != nil {
		t.Fatal(err)
	}
	// Drain an intentionally reproduced leak before the fixture pool cleanup.
	defer func() {
		handler.txs.mu.Lock()
		defer handler.txs.mu.Unlock()
		for id, transaction := range handler.txs.open {
			cleanup, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_ = transaction.tx.Rollback(cleanup)
			cancel()
			transaction.conn.Release()
			delete(handler.txs.open, id)
		}
	}()
	key := time.Now().UnixNano()
	acquired := false
	family := domain.Family{Name: "commit_cleanup_fixture", Stage: 1, Ops: map[uint32]domain.Op{1: {Name: "cancel_after_final_query", Args: 0, Tx: true, Timeout: 200 * time.Millisecond, Run: func(ctx context.Context, q domain.Queryer, _ []string) (uint32, []string, error) {
		if _, err := q.Exec(ctx, `UPDATE commit_cleanup_owner SET id=id WHERE id=1`); err != nil {
			return 0, nil, err
		}
		if _, err := q.Exec(ctx, "SELECT pg_advisory_xact_lock($1)", key); err != nil {
			return 0, nil, err
		}
		acquired = true
		<-ctx.Done()
		return domain.StatusOK, nil, nil
	}}}}
	request := make([]byte, 8)
	binary.LittleEndian.PutUint32(request, 1)
	reply, transport := family.Handler(store)(bus.ModuleInvocation{StageID: 1}, request)
	if transport != bus.ModuleStatusInternal || !acquired || len(reply) != 0 {
		t.Fatal("cancelled commit error changed", transport, acquired, reply)
	}
	handler.txs.mu.Lock()
	live := len(handler.txs.open)
	handler.txs.mu.Unlock()
	if live != 0 || pool.Stat().AcquiredConns() != 0 {
		t.Fatalf("cancel-before-commit retained owner: handles=%d connections=%d commit_attempts=%d rollback_attempts=%d", live, pool.Stat().AcquiredConns(), caller.commits, caller.rollbacks)
	}
	if caller.commits != 1 || caller.rollbacks != 1 {
		t.Fatal("commit retried or rollback omitted", caller.commits, caller.rollbacks)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	probe, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer probe.Rollback(context.Background())
	var available bool
	if err = probe.QueryRow(ctx, "SELECT pg_try_advisory_xact_lock($1)", key).Scan(&available); err != nil || !available {
		t.Fatal("exact transaction lock retained", available, err)
	}
	if _, err = probe.Exec(ctx, `UPDATE commit_cleanup_owner SET id=id WHERE id=1`); err != nil {
		t.Fatal("owner row retained", err)
	}
}

func TestLostCommitAcknowledgmentNeverRetriesOrClaimsRollback(t *testing.T) {
	pool := sharedTestDatabase(t)
	if _, err := pool.Exec(context.Background(), `CREATE TABLE unknown_commit_effect(value integer);INSERT INTO unknown_commit_effect VALUES(0)`); err != nil {
		t.Fatal(err)
	}
	handler := &sqlHandler{txs: newTxRegistry(), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	caller := &commitCleanupCaller{sharedStoreCaller: sharedStoreCaller{handler.handle}, lostCommitAck: true}
	store, err := db.NewStore(caller)
	if err != nil {
		t.Fatal(err)
	}
	family := domain.Family{Name: "lost_commit_ack_fixture", Stage: 1, Ops: map[uint32]domain.Op{1: {Name: "commit_once", Args: 0, Tx: true, Run: func(ctx context.Context, q domain.Queryer, _ []string) (uint32, []string, error) {
		_, err := q.Exec(ctx, `UPDATE unknown_commit_effect SET value=value+1`)
		return domain.StatusOK, nil, err
	}}}}
	request := make([]byte, 8)
	binary.LittleEndian.PutUint32(request, 1)
	reply, transport := family.Handler(store)(bus.ModuleInvocation{StageID: 1}, request)
	if transport != bus.ModuleStatusInternal || len(reply) != 0 {
		t.Fatal("unknown commit was reported successful", transport, reply)
	}
	handler.txs.mu.Lock()
	live := len(handler.txs.open)
	handler.txs.mu.Unlock()
	if caller.commits != 1 || caller.rollbacks != 1 || live != 0 || pool.Stat().AcquiredConns() != 0 {
		t.Fatal("ambiguous commit retried or retained owner", caller.commits, caller.rollbacks, live)
	}
	var actual int
	if err = pool.QueryRow(context.Background(), `SELECT value FROM unknown_commit_effect`).Scan(&actual); err != nil || actual != 1 {
		t.Fatal("cleanup falsely assumed commit never executed", actual, err)
	}
}

type restartedCommitCaller struct {
	current, next    sharedStoreCaller
	newTx            db.Tx
	restarted        bool
	rollbackAttempts int
}

func (c *restartedCommitCaller) Call(ctx context.Context, kind, stage uint32, id uint64, deadline time.Duration, request []byte) ([]byte, error) {
	op := binary.LittleEndian.Uint32(request)
	if op == opCommit && !c.restarted {
		// The old provider committed, then disappeared before returning its ACK.
		if _, err := c.current.Call(ctx, kind, stage, id, deadline, request); err != nil {
			return nil, err
		}
		c.restarted = true
		c.current = c.next
		// A distinct client begins its own lease on the replacement provider before
		// the old client's automatic cleanup reaches it.
		replacement, err := db.NewStore(c.next)
		if err != nil {
			return nil, err
		}
		c.newTx, err = replacement.Begin(ctx)
		if err != nil {
			return nil, err
		}
		if _, err = c.newTx.Exec(ctx, `UPDATE replacement_owner_effect SET value=value+1`); err != nil {
			return nil, err
		}
		return nil, errors.New("old provider disappeared before commit acknowledgment")
	}
	if op == opRollback {
		c.rollbackAttempts++
	}
	return c.current.Call(ctx, kind, stage, id, deadline, request)
}

func TestOldCommitCleanupCannotFinishReplacementProviderTransaction(t *testing.T) {
	pool := sharedTestDatabase(t)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err := pool.Exec(ctx, `CREATE TABLE replacement_owner_effect(value integer);INSERT INTO replacement_owner_effect VALUES(0)`); err != nil {
		t.Fatal(err)
	}
	old := &sqlHandler{txs: newTxRegistryWithEntropy(transactionSeed(100)), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	replacement := &sqlHandler{txs: newTxRegistryWithEntropy(transactionSeed((uint64(1) << 63) + 900)), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }}
	caller := &restartedCommitCaller{current: sharedStoreCaller{old.handle}, next: sharedStoreCaller{replacement.handle}}
	store, err := db.NewStore(caller)
	if err != nil {
		t.Fatal(err)
	}
	tx, err := store.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err = tx.Commit(ctx); err == nil {
		t.Fatal("lost old-provider ACK became success")
	}
	if caller.newTx == nil {
		t.Fatal("replacement lease was not created")
	}
	defer caller.newTx.Rollback(context.Background())
	if !caller.restarted || caller.rollbackAttempts != 1 {
		t.Fatal("old cleanup did not reach replacement provider exactly once")
	}
	if _, ok := replacement.txs.get((uint64(1) << 63) + 901); !ok {
		t.Fatal("old handle consumed new provider's transaction")
	}
	var own, visible int
	if err = caller.newTx.QueryRow(ctx, `SELECT value FROM replacement_owner_effect`).Scan(&own); err != nil || own != 1 {
		t.Fatal("new owner was closed by stale cleanup", own, err)
	}
	if err = pool.QueryRow(ctx, `SELECT value FROM replacement_owner_effect`).Scan(&visible); err != nil || visible != 0 {
		t.Fatal("new effect was prematurely committed", visible, err)
	}
	if err = caller.newTx.Commit(ctx); err != nil {
		t.Fatal("new owner could not finish its own lease", err)
	}
	if err = pool.QueryRow(ctx, `SELECT value FROM replacement_owner_effect`).Scan(&visible); err != nil || visible != 1 {
		t.Fatal("new owner lost its effect", visible, err)
	}
}
