package postgres

import (
	"context"
	"fmt"
	"log"

	"github.com/JBailes/aimee/server-go/db3"
)

// Replication: getting PostgreSQL's committed writes into the provisioned
// vector database.
//
// A provider that is searched but never written to answers correctly and
// emptily forever, which reads as a corpus with no matches rather than one
// nobody filled. This is the half that fills it.
//
// The order is not negotiable. An operation is written to the outbox in the
// SAME transaction that commits the row, and published only after that commits
// -- so a provider can never hold a row PostgreSQL does not. The reverse would
// let a crash between publish and commit leave the vector store holding a row
// that never existed, and nothing would ever correct it, because the outbox
// would have no record of an operation that was never written.

// RunVectorReplication ships committed operations to the provisioned provider
// until ctx ends.
//
// Returns immediately when no vector database was provisioned: there is nothing
// to replicate to, and that is the ordinary deployment rather than an error.
func RunVectorReplication(ctx context.Context, attachment *VectorBus, leaseOwner string) error {
	if attachment == nil || attachment.Provider().Principal == 0 {
		return nil
	}
	if ctx == nil {
		ctx = context.Background()
	}
	pool, err := SQLPool(ctx)
	if err != nil || pool == nil {
		return fmt.Errorf("postgres: vector replication has no database: %w", err)
	}
	store, err := NewPGDB3Outbox(pool, leaseOwner)
	if err != nil {
		return err
	}
	return RunDB3Outbox(ctx, store, attachment)
}

// StartVectorReplication runs replication in the background, logging what it
// decided.
//
// Logged rather than silent because "no vector database provisioned" and
// "replication stopped" look identical from outside -- both are simply an
// absence of traffic -- and an operator who installed a vector database needs
// to know which one they have.
func StartVectorReplication(ctx context.Context, attachment *VectorBus, leaseOwner string) {
	if attachment == nil || attachment.Provider().Principal == 0 {
		return
	}
	go func() {
		err := RunVectorReplication(ctx, attachment, leaseOwner)
		if err != nil && ctx.Err() == nil {
			log.Printf("postgres: vector replication to %q stopped: %v",
				attachment.Provider().Instance, err)
		}
	}()
}

// AppliedObserver routes a provider's acknowledgements into the outbox ledger.
//
// Exposed so whoever owns the bus attachment can hand Applied notifications
// here: they are the durable completion evidence, and an operation the provider
// never acknowledged is one the outbox must redeliver.
func (v *VectorBus) AppliedObserver(ctx context.Context,
	store *PGDB3Outbox) func(uint32, db3.Applied) {
	if store == nil {
		return func(uint32, db3.Applied) {}
	}
	return store.AppliedObserver(ctx)
}
