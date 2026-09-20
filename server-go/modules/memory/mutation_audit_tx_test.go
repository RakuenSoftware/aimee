package memory

import (
	"context"
	"errors"
	"testing"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/audit"
)

type auditTestTx struct {
	store.Queryer
	committed, rolled bool
	failure           error
}

func (tx *auditTestTx) Commit(context.Context) error {
	if tx.committed || tx.rolled {
		return store.ErrTxClosed
	}
	if tx.failure != nil {
		return tx.failure
	}
	tx.committed = true
	return nil
}
func (tx *auditTestTx) Rollback(context.Context) error {
	if tx.committed || tx.rolled {
		return store.ErrTxClosed
	}
	tx.rolled = true
	return nil
}

func TestMutationAuditTransaction(t *testing.T) {
	ctx := context.Background()
	for _, mode := range []string{"commit", "rollback", "failed-commit"} {
		t.Run(mode, func(t *testing.T) {
			inner := &auditTestTx{}
			if mode == "failed-commit" {
				inner.failure = errors.New("commit failed")
			}
			var actions []audit.Action
			s := &postgresDataStore{auditAction: func(_ context.Context, a audit.Action) error {
				if !inner.committed {
					t.Fatal("observation before commit")
				}
				actions = append(actions, a)
				return nil
			}}
			tx := s.auditTransaction(inner)
			s.db = tx
			s.recordMutation(DataRequest{Operation: "insert-epistemic"}, DataResponse{Records: []Record{{ID: 7, Kind: "fact", Key: "private-key"}}}, nil, "")
			s.recordMutation(DataRequest{Operation: "update-content", ID: 7}, DataResponse{Updated: true}, nil, "")
			if len(actions) != 0 {
				t.Fatal("early publication")
			}
			if mode != "rollback" {
				if err := tx.Commit(ctx); (err == nil) != (mode == "commit") {
					t.Fatal(err)
				}
			}
			_ = tx.Rollback(ctx)
			want := 0
			if mode == "commit" {
				want = 2
			}
			if len(actions) != want {
				t.Fatal(actions)
			}
			_ = tx.Commit(ctx)
			_ = tx.Rollback(ctx)
			if len(actions) != want {
				t.Fatal("duplicate observations")
			}
		})
	}
}

func TestMutationAuditRequestBatch(t *testing.T) {
	var actions []audit.Action
	s := &postgresDataStore{auditBatch: &mutationAuditBatch{}, auditAction: func(_ context.Context, a audit.Action) error { actions = append(actions, a); return nil }}
	outer := audit.Action{Tool: "outer"}
	s.auditBatch.add(outer)
	tx := s.auditTransaction(&auditTestTx{})
	bound := *s
	bound.db = tx
	bound.recordMutation(DataRequest{Operation: "reject", ID: 1}, DataResponse{Updated: true}, nil, "")
	if err := tx.Rollback(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(s.auditBatch.actions) != 1 || len(actions) != 0 {
		t.Fatal("rollback crossed enclosing batch")
	}
	tx = s.auditTransaction(&auditTestTx{})
	bound.db = tx
	bound.recordMutation(DataRequest{Operation: "reject", ID: 2}, DataResponse{Updated: true}, nil, "")
	if err := tx.Commit(context.Background()); err != nil {
		t.Fatal(err)
	}
	_ = tx.Rollback(context.Background())
	if len(actions) != 0 {
		t.Fatal("nested commit published before enclosing request")
	}
	s.auditBatch.flush(s.auditAction)
	if len(actions) != 2 || actions[0].Tool != "outer" || actions[1].TaskID != 2 {
		t.Fatal(actions)
	}
}

func TestMutationAuditBatchBound(t *testing.T) {
	b := &mutationAuditBatch{}
	for i := 0; i < mutationAuditBatchLimit+5; i++ {
		b.add(audit.Action{TaskID: int64(i)})
	}
	if len(b.actions) != mutationAuditBatchLimit || b.dropped != 5 {
		t.Fatal(len(b.actions), b.dropped)
	}
}
