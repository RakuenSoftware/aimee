package memory

import (
	"context"
	"log"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/audit"
)

const mutationAuditBatchLimit = 4096

type mutationAuditBatch struct {
	actions []audit.Action
	dropped int
}

func (b *mutationAuditBatch) add(a audit.Action) {
	if len(b.actions) == mutationAuditBatchLimit {
		b.dropped++
		return
	}
	b.actions = append(b.actions, a)
}
func (b *mutationAuditBatch) flush(publish func(context.Context, audit.Action) error) {
	if publish == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()
	failed := b.dropped
	for _, a := range b.actions {
		if err := publish(ctx, a); err != nil {
			failed++
		}
	}
	if failed > 0 {
		log.Printf("memory audit: %d mutation observations not delivered", failed)
	}
	b.actions = nil
	b.dropped = 0
}

// Buffer observations with the actual transaction. A later lineage/job failure
// must discard earlier successful statements; only Commit may release them.
type mutationAuditTx struct {
	store.Tx
	batch           *mutationAuditBatch
	publish         func(context.Context, audit.Action) error
	ownsBatch, done bool
	start           int
	droppedStart    int
}

func (s *postgresDataStore) auditTransaction(tx store.Tx) store.Tx {
	if s.auditAction == nil {
		return tx
	}
	batch := s.auditBatch
	owns := batch == nil
	if owns {
		batch = &mutationAuditBatch{}
	}
	return &mutationAuditTx{Tx: tx, batch: batch, publish: s.auditAction, ownsBatch: owns, start: len(batch.actions), droppedStart: batch.dropped}
}
func (tx *mutationAuditTx) Commit(ctx context.Context) error {
	err := tx.Tx.Commit(ctx)
	if err == nil && !tx.done {
		tx.done = true
		if tx.ownsBatch {
			tx.batch.flush(tx.publish)
		}
	}
	return err
}
func (tx *mutationAuditTx) Rollback(ctx context.Context) error {
	err := tx.Tx.Rollback(ctx)
	if !tx.done {
		tx.done = true
		tx.batch.actions = tx.batch.actions[:tx.start]
		tx.batch.dropped = tx.droppedStart
	}
	return err
}
func (s *postgresDataStore) recordMutation(request DataRequest, response DataResponse, err error, tool string) {
	if err != nil || s.auditAction == nil {
		return
	}
	a, ok := mutationAudit(request, response, bus.ModuleStatusOK)
	if !ok || a.Verdict != "ok" {
		return
	}
	if tool != "" {
		a.Tool = tool
	}
	batch := s.auditBatch
	if tx, ok := s.db.(*mutationAuditTx); ok {
		batch = tx.batch
	}
	if batch != nil {
		batch.add(a)
		return
	}
	if _, ok := s.db.(store.Tx); ok {
		log.Printf("memory audit: caller-owned transaction lacks observation binding")
		return
	}
	// A direct single-statement mutation on the store has already autocommitted.
	batch = &mutationAuditBatch{}
	batch.add(a)
	batch.flush(s.auditAction)
}

// SQL savepoint rollback must rewind observations as well as rows: background
// workers intentionally commit their retry bookkeeping after failed work.
func (s *postgresDataStore) auditSavepoint() func() {
	batch := s.auditBatch
	if tx, ok := s.db.(*mutationAuditTx); ok {
		batch = tx.batch
	}
	if batch == nil {
		return func() {}
	}
	count, dropped := len(batch.actions), batch.dropped
	return func() { batch.actions = batch.actions[:count]; batch.dropped = dropped }
}
