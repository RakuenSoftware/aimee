package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"testing"
)

type deletionRetryDB struct {
	store.Queryer
	failures      []error
	commitFailure bool
	transactions  []*deletionRetryTx
}

func (db *deletionRetryDB) Begin(context.Context) (store.Tx, error) {
	for _, tx := range db.transactions {
		if !tx.done {
			panic("new transaction before rollback")
		}
	}
	tx := &deletionRetryTx{commitFailure: db.commitFailure}
	if len(db.transactions) < len(db.failures) {
		tx.failure = db.failures[len(db.transactions)]
	}
	db.transactions = append(db.transactions, tx)
	return tx, nil
}

type deletionRetryTx struct {
	store.Queryer
	failure                        error
	commitFailure, done, committed bool
}

func (tx *deletionRetryTx) Exec(context.Context, string, ...any) (store.Tag, error) {
	return store.RowsAffected(1), nil
}
func (tx *deletionRetryTx) QueryRow(context.Context, string, ...any) store.Row { return tx }
func (tx *deletionRetryTx) Scan(dest ...any) error {
	if !tx.commitFailure && tx.failure != nil {
		return tx.failure
	}
	*dest[0].(*bool) = true
	return nil
}
func (tx *deletionRetryTx) Commit(context.Context) error {
	if tx.commitFailure && tx.failure != nil {
		return tx.failure
	}
	tx.done, tx.committed = true, true
	return nil
}
func (tx *deletionRetryTx) Rollback(context.Context) error { tx.done = true; return nil }
func TestDeletionDeadlockRetry(t *testing.T) {
	deadlock := fmt.Errorf("wrapped: %w", &store.StoreError{SQLState: "40P01"})
	for _, test := range []struct {
		name          string
		failures      []error
		commitFailure bool
		attempts      int
		status        bus.ModuleStatus
	}{
		{name: "statement deadlock", failures: []error{deadlock}, attempts: 2, status: bus.ModuleStatusOK},
		{name: "commit deadlock", failures: []error{deadlock}, commitFailure: true, attempts: 2, status: bus.ModuleStatusOK},
		{name: "exhausted", failures: []error{deadlock, deadlock, deadlock}, attempts: 3, status: bus.ModuleStatusInternal},
		{name: "other SQL failure", failures: []error{&store.StoreError{SQLState: "23503"}}, attempts: 1, status: bus.ModuleStatusInternal},
		{name: "success", attempts: 1, status: bus.ModuleStatusOK},
	} {
		t.Run(test.name, func(t *testing.T) {
			db := &deletionRetryDB{failures: test.failures, commitFailure: test.commitFailure}
			published := 0
			backend := &postgresDataStore{placement: PlacementKB, db: db, auditAction: func(_ context.Context, action audit.Action) error {
				if action.Verdict != "ok" {
					return nil
				}
				if !db.transactions[len(db.transactions)-1].committed {
					t.Fatal("audit before commit")
				}
				published++
				return nil
			}}
			body, _ := json.Marshal(DataRequest{Operation: "delete-as", ID: 7, Authority: AuthorityUser, Scope: Scope{Type: ScopeProject, Value: "retry"}})
			raw, status := handleData(handlerOptions{placement: PlacementKB, data: backend}, bus.ModuleInvocation{}, body)
			if status != test.status || len(db.transactions) != test.attempts {
				t.Fatalf("status=%v attempts=%d", status, len(db.transactions))
			}
			for _, tx := range db.transactions {
				if !tx.done {
					t.Fatal("transaction left open")
				}
			}
			want := 0
			if status == bus.ModuleStatusOK {
				var response DataResponse
				if json.Unmarshal(raw, &response) != nil || !response.Deleted {
					t.Fatal(string(raw))
				}
				want = 1
			}
			if published != want {
				t.Fatalf("published %d audits, want %d", published, want)
			}
		})
	}
}
func TestDeletionDeadlockDoesNotRetryCallerTransaction(t *testing.T) {
	tx := &deletionRetryTx{failure: &store.StoreError{SQLState: "40P01"}}
	body, _ := json.Marshal(DataRequest{Operation: "delete-as", ID: 7, Authority: AuthorityUser, Scope: Scope{Type: ScopeProject, Value: "retry"}})
	_, status := handleData(handlerOptions{placement: PlacementKB, data: &postgresDataStore{placement: PlacementKB, db: tx}}, bus.ModuleInvocation{}, body)
	if status != bus.ModuleStatusInternal || tx.done {
		t.Fatal("caller transaction ownership changed", status, tx.done)
	}
}

func TestDeletionDeadlockHonorsCancelledRequest(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	db := &deletionRetryDB{failures: []error{&store.StoreError{SQLState: "40P01"}}}
	body, _ := json.Marshal(DataRequest{Operation: "delete-as", ID: 7, Authority: AuthorityUser, Scope: Scope{Type: ScopeProject, Value: "retry"}})
	_, status := handleData(handlerOptions{dataContext: ctx, placement: PlacementKB, data: &postgresDataStore{placement: PlacementKB, db: db}}, bus.ModuleInvocation{}, body)
	if status != bus.ModuleStatusCancelled || len(db.transactions) != 1 || !db.transactions[0].done {
		t.Fatal("cancelled request retried or retained its transaction", status, len(db.transactions))
	}
}
