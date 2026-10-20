package postgres

import (
	"context"
	"encoding/binary"
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
