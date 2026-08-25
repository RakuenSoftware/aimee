package postgres

import (
	"context"
	"fmt"
	"log"
	"os"
	"strconv"

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
	// Installed BEFORE the first publish. An operation published while nothing
	// was listening would be acknowledged into a dropped event and then sit
	// claimed until its lease expired, so the first pass would replay entirely.
	attachment.ObserveApplied(attachment.AppliedObserver(ctx, store))
	defer attachment.ObserveApplied(nil)
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

// ReplicationLeaseOwner names this process in the outbox ledger.
//
// The outbox hands out claims by lease, so the name must be UNIQUE PER PROCESS:
// two replicators sharing one name would each renew the other's claims, and a
// crash would leave rows that look continuously claimed by a live owner and are
// never redelivered. Host and pid together give that without configuration.
//
// Reduced to printable ASCII within the column's 64 bytes, because a hostname
// is whatever the operator set it to and the ledger refuses anything else.
func ReplicationLeaseOwner() string {
	host, err := os.Hostname()
	if err != nil || host == "" {
		host = "unknown"
	}
	sanitized := make([]byte, 0, len(host))
	for index := range len(host) {
		character := host[index]
		if character < 0x21 || character > 0x7e {
			character = '-'
		}
		sanitized = append(sanitized, character)
	}
	owner := "postgres@" + string(sanitized) + "/" + strconv.Itoa(os.Getpid())
	if len(owner) > 64 {
		owner = owner[:64]
	}
	return owner
}
