package aimee

import (
	"context"
	"errors"
	"testing"
)

// wroteRow answers wroteSQL.
type wroteRow struct{ wrote bool }

func (r wroteRow) Scan(dest ...any) error {
	if len(dest) != 1 {
		return errors.New("one column")
	}
	*dest[0].(*bool) = r.wrote
	return nil
}

// writeTx is a transaction that knows whether its body wrote.
type writeTx struct {
	fakeTx
	wrote bool
	asked bool
}

func (t *writeTx) QueryRow(_ context.Context, sql string, _ ...any) Row {
	if sql == wroteSQL {
		t.asked = true
		return wroteRow{wrote: t.wrote}
	}
	return nil
}

type writeDB struct {
	fakeDB
	tx *writeTx
}

func (d *writeDB) Begin(context.Context) (Tx, error) { return d.tx, nil }

func changedAfter(t *testing.T, op Op, tx *writeTx) []string {
	t.Helper()
	var changed []string
	family := Family{Changed: func(name string) { changed = append(changed, name) }}
	op.Name = "work_item_set_pause"
	_, _, _ = family.run(context.Background(), &writeDB{tx: tx}, op, nil)
	return changed
}

func ok(context.Context, Queryer, []string) (uint32, []string, error) {
	return StatusOK, nil, nil
}

func TestACommittedWriteIsAnnounced(t *testing.T) {
	tx := &writeTx{wrote: true}
	if got := changedAfter(t, Op{Tx: true, Run: ok}, tx); len(got) != 1 || got[0] != "work_item_set_pause" {
		t.Fatalf("announced %v, want the operation once", got)
	}
	if !tx.committed {
		t.Fatal("the transaction did not commit")
	}
}

func TestATransactionThatWroteNothingIsNotAnnounced(t *testing.T) {
	// A sweep that matched no row: announcing it would wake its consumer, whose
	// next sweep would announce again, forever.
	tx := &writeTx{wrote: false}
	if got := changedAfter(t, Op{Tx: true, Run: ok}, tx); len(got) != 0 {
		t.Fatalf("announced %v for a transaction that wrote nothing", got)
	}
	if !tx.asked || !tx.committed {
		t.Fatal("the write question must be asked before the commit")
	}
}

func TestARefusedOrFailedOperationIsNotAnnounced(t *testing.T) {
	refused := func(context.Context, Queryer, []string) (uint32, []string, error) {
		return StatusInvalid, nil, nil
	}
	failed := func(context.Context, Queryer, []string) (uint32, []string, error) {
		return 0, nil, errors.New("boom")
	}
	for _, body := range []OpFunc{refused, failed} {
		if got := changedAfter(t, Op{Tx: true, Run: body}, &writeTx{wrote: true}); len(got) != 0 {
			t.Fatalf("announced %v for a rolled-back transaction", got)
		}
	}
}

func TestAnOperationOwningItsTransactionIsAnnouncedToo(t *testing.T) {
	owned := Op{RunDB: func(ctx context.Context, db DB, fields []string) (uint32, []string, error) {
		tx, err := db.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		return StatusOK, nil, tx.Commit(ctx)
	}}
	if got := changedAfter(t, owned, &writeTx{wrote: true}); len(got) != 1 {
		t.Fatalf("announced %v, want once", got)
	}
}

func TestAReadOpensNoTransactionAndIsNotAnnounced(t *testing.T) {
	tx := &writeTx{wrote: true}
	if got := changedAfter(t, Op{Run: ok}, tx); len(got) != 0 || tx.asked {
		t.Fatalf("a read announced %v (asked=%v)", got, tx.asked)
	}
}

func notedAfter(t *testing.T, body OpFunc) []string {
	t.Helper()
	var noted []string
	family := Family{Noted: func(topic string) { noted = append(noted, topic) }}
	_, _, _ = family.run(context.Background(), &writeDB{tx: &writeTx{wrote: true}},
		Op{Name: "session_ir_append", Tx: true, Run: body}, nil)
	return noted
}

func TestANoteIsDeliveredOnceAfterTheCommit(t *testing.T) {
	queued := func(ctx context.Context, _ Queryer, _ []string) (uint32, []string, error) {
		Note(ctx, "learning_projection_queued")
		Note(ctx, "learning_projection_queued")
		return StatusOK, nil, nil
	}
	if got := notedAfter(t, queued); len(got) != 1 || got[0] != "learning_projection_queued" {
		t.Fatalf("noted %v, want the topic once", got)
	}
}

func TestARolledBackNoteIsNotDelivered(t *testing.T) {
	refused := func(ctx context.Context, _ Queryer, _ []string) (uint32, []string, error) {
		Note(ctx, "learning_projection_queued")
		return StatusInvalid, nil, nil
	}
	if got := notedAfter(t, refused); len(got) != 0 {
		t.Fatalf("noted %v for a refused operation", got)
	}
	Note(context.Background(), "nowhere") // outside an operation: nothing to do, no panic
}
