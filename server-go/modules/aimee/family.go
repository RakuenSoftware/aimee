package aimee

import (
	"context"
	"errors"
	"log"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// DefaultTimeout bounds one operation's database work when the invocation
// carries no deadline of its own.
const DefaultTimeout = 2 * time.Second

// OpFunc runs one operation. It returns the status the caller sees plus the
// reply fields. A returned error is the module's own failure -- a broken query,
// a lost connection -- and becomes StatusFailed; a refusal the caller should
// understand (a missing row, a bad argument) is a status, not an error.
type OpFunc func(ctx context.Context, q Queryer, fields []string) (uint32, []string, error)

// OpDBFunc runs an operation that manages its own transaction.
//
// Most operations should not: Op.Tx wraps them, and one shape for every family
// is worth more than local cleverness. It exists for the operations whose
// COMMIT/ROLLBACK decision is part of their answer rather than a consequence of
// it -- the jti stores commit a successful garbage-collection pass while
// rolling back a replay, so "did this succeed" and "should this persist" are
// genuinely two questions there.
type OpDBFunc func(ctx context.Context, db DB, fields []string) (uint32, []string, error)

// Op is one operation in a family.
type Op struct {
	// Name is the catalog's name for this op, used only in logs.
	Name string
	// Args is the exact number of request fields the op expects. A frame with
	// the wrong count is StatusInvalid rather than a panic in the op body, and
	// stating it here means no op has to re-check its own arity. Negative means
	// variadic, which an op then validates itself.
	Args int
	// Cells is how many cells ONE reply row carries, for an operation whose
	// reply is a list of rows. Zero means the operation does not answer with
	// rows at all.
	//
	// This is checked against the catalog's declared reply, which is the only
	// thing that catches a reply built to the shape the schema suggested rather
	// than the shape the contract states. A behavioural test cannot catch it:
	// the test would be written to the same wrong shape as the code.
	//
	// Keep it and the width passed to collect() the same named constant, so
	// they cannot drift into agreeing with each other and disagreeing with the
	// wire.
	Cells int
	// Tx runs the op inside a transaction. Operations the catalog marks
	// transaction:"single" set this; the transaction is one operation wide,
	// never spanning two calls, because a transaction assembled across a wire
	// is not a transaction.
	Tx bool
	// Run is the body. Exactly one of Run and RunDB must be set.
	Run OpFunc
	// RunDB is the body for an operation that manages its own transaction.
	// Setting it means Tx is ignored: the operation is the thing deciding.
	RunDB OpDBFunc
}

// Family is one event kind's worth of operations.
type Family struct {
	Name  string
	Event uint32
	Stage uint32
	Ops   map[uint32]Op
}

// Handler builds the bus handler for this family against a database.
func (f Family) Handler(db DB) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != f.Stage {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if db == nil {
			return nil, bus.ModuleStatusInternal
		}
		op, fields, ok := DecodeRequest(frame)
		if !ok {
			// A malformed frame is not a question, so it is refused at the bus
			// level. Everything the module can actually answer is answered
			// in-band, below, with a status.
			return nil, bus.ModuleStatusInvalidRequest
		}
		spec, known := f.Ops[op]
		if !known {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if !ValidFields(fields) {
			return Status(StatusInvalid), bus.ModuleStatusOK
		}
		if spec.Args >= 0 && len(fields) != spec.Args {
			return Status(StatusInvalid), bus.ModuleStatusOK
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		timeout := invocation.Remaining(DefaultTimeout)
		if timeout <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()

		status, reply, err := f.run(ctx, db, spec, fields)
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if err != nil {
			// The error text is for this process's log, never the wire: a driver
			// error can quote connection details or row contents.
			log.Printf("aimee: %s/%s failed: %v", f.Name, spec.Name, err)

			// AN UNREACHABLE STORE IS NOT A FAILED REQUEST, and collapsing the
			// two is the defect this module exists to avoid one layer down.
			//
			// StatusFailed says "I understood your request and it did not
			// work" -- a fact about the request, and a caller is right to
			// record it and move on. "The postgres module is not answering" is
			// a fact about the MOMENT: the same request will succeed once the
			// store is back, and a caller that writes the failure down as an
			// outcome has persisted a wrong conclusion from a transient fault.
			//
			// That is not hypothetical for a store. Every operation in the
			// system arrives here, so during a postgres-module restart every
			// caller would otherwise be told its own request was bad -- 463
			// operations all individually wrong at the same instant, which is
			// the one explanation that cannot be true.
			//
			// Reported at the TRANSPORT level rather than as a sixth status.
			// The five are the catalog's result codes, shared with 461 C call
			// sites, and a family does not get to invent another. ModuleStatus
			// is where "I cannot serve this right now" already lives -- the
			// runtime uses Internal for its in-flight cap for the same reason
			// -- and it costs C callers nothing, since call_stage returns -1
			// for a transport failure and for a refused status alike.
			if errors.Is(err, ErrStoreUnavailable) {
				return nil, bus.ModuleStatusInternal
			}
			return Status(StatusFailed), bus.ModuleStatusOK
		}
		// A reply that is not a whole number of rows is a contract break, and
		// the client end of it is unreadable: call_stage treats an OK reply
		// narrower than the slots it asked for as a mismatch and returns -1,
		// giving the caller a bare failure with nothing to debug from. Ten ops
		// shipped exactly that way -- one cell where the C had always emitted
		// the value AND its rc -- because Cells was only ever compared against
		// the catalog, never against what a handler actually produced. Failing
		// here names the op instead.
		if status == StatusOK && spec.Cells > 0 && len(reply)%spec.Cells != 0 {
			log.Printf("aimee: %s/%s answered with %d cells, not a multiple of the %d it declares",
				f.Name, spec.Name, len(reply), spec.Cells)
			return Status(StatusFailed), bus.ModuleStatusOK
		}
		return EncodeReply(status, reply), bus.ModuleStatusOK
	}
}

// run executes one op, inside a transaction when the op declares one.
// statementTagger is a store that can attribute statements to an operation.
// An assertion rather than part of DB, so a fake store in a test stays the four
// methods it needs and does not have to care.
type statementTagger interface {
	WithStatementID(id string) DB
}

func (f Family) run(ctx context.Context, db DB, spec Op, fields []string) (uint32, []string, error) {
	// Everything this operation runs is attributed to it, including inside a
	// transaction: the store sees "runtime_state_get did this", not "some
	// connection did this".
	if tagger, ok := db.(statementTagger); ok && spec.Name != "" {
		db = tagger.WithStatementID(spec.Name)
	}
	if spec.RunDB != nil {
		return spec.RunDB(ctx, db, fields)
	}
	if spec.Run == nil {
		return 0, nil, errors.New("aimee: operation has no body")
	}
	if !spec.Tx {
		return spec.Run(ctx, db, fields)
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	status, reply, err := spec.Run(ctx, tx, fields)
	if err != nil {
		// Rollback failure is not reported over the top of the error that
		// caused it: the first error is the one that explains the outcome.
		_ = tx.Rollback(ctx)
		return 0, nil, err
	}
	if status != StatusOK {
		// A refusal must not leave a partial write behind. An op that reports
		// "missing" or "invalid" after having written something would otherwise
		// commit that write, which is the sharpest way to corrupt a store.
		// A rollback of an already-finished transaction is not a failure: the
		// refusal path can be reached after the store has closed it. aimee's own
		// sentinel, so this line does not name a database library either.
		if err := tx.Rollback(ctx); err != nil && !errors.Is(err, ErrTxClosed) {
			return 0, nil, err
		}
		return status, reply, nil
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return status, reply, nil
}

// The connection is NOT aimee's. Opening a database here would make aimee a second
// process holding the store, which is exactly what moving it behind a module
// removed. The postgres module owns the pool, the DSN and the pooling policy;
// aimee asks it over the bus through the shared server-go/db client, also used
// by KB and server memory. This module contains no database wire implementation.
