package postgres

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Open transactions, held on behalf of the callers that opened them.
//
// A transaction pins a connection, so every one of these is a resource borrowed
// from the pool until the caller gives it back or stops existing. That makes
// reclaim the load-bearing part of this file, not the handle format.
//
// WHAT GOES WRONG IF RECLAIM IS QUIET. A caller whose transaction was reclaimed
// keeps issuing statements against the handle, and then commits. If those
// answer OK, every write in the transaction is silently lost and the caller is
// told it succeeded. So a statement on a handle this no longer holds answers
// SQLSTATE 25P01 (no_active_sql_transaction) -- including COMMIT, especially
// COMMIT -- which turns silent data loss into a visible error the caller
// already knows how to handle.
//
// 25P01 and an ordinary statement failure are not the same answer and callers
// act on the difference: a unique violation (23505) leaves the transaction live
// with one statement wrong, while 25P01 means everything in it is already gone
// and the work has to start over. Collapsing them lets a caller log "that
// insert failed, carrying on" and then commit nothing.
const (
	// SQLSTATE the caller maps to "this transaction is gone".
	sqlStateNoActiveTransaction = "25P01"
	// The transaction exists but every statement in it will fail until it is
	// rolled back. Distinct from being reclaimed: the work is still recoverable
	// by starting over, but the handle is not a lie.
	sqlStateInFailedTransaction = "25P02"

	// A caller is holding as many transactions as it may. too_many_connections
	// is the honest code: a transaction pins a pooled connection, so this is a
	// connection cap wearing another name.
	//
	// Distinct from the result-size refusal below, because the two want
	// different responses and share a status. A leaked handle costs a caller
	// capacity permanently, and its first symptom is unrelated operations
	// failing -- an indistinguishable refusal sends whoever investigates into
	// the wrong module.
	sqlStateTooManyTransactions = "53300"
	// The answer is larger than the wire will carry. Refused rather than
	// truncated: a caller handed exactly the ceiling cannot tell a capped
	// answer from a complete one.
	sqlStateResultTooLarge = "54000"
	// A statement that did not finish, and a connection that did not answer.
	// 57014 is what PostgreSQL itself returns when statement_timeout fires, so a
	// caller-side deadline reports the same fact the server would have.
	sqlStateQueryCanceled     = "57014"
	sqlStateConnectionFailure = "08006"

	// How long a transaction may sit idle before it is reclaimed. A caller that
	// has stopped talking is holding a pooled connection hostage, and the pool
	// is shared with every other module.
	transactionIdleTimeout = 30 * time.Second
	// A ceiling per caller, so one module cannot drain the pool by opening
	// transactions it never closes.
	maxOpenPerPrincipal = 16
)

type openTransaction struct {
	tx pgx.Tx
	// Released back to the pool when the transaction ends.
	conn *pgxpool.Conn

	// Who opened it. A statement from anyone else is refused: the handle names
	// a transaction, it does not confer the right to drive one.
	principal uint32
	// Which attachment opened it, so a reconnecting principal does not inherit
	// the transactions of its previous attachment.
	handle uint32

	lastUsed time.Time
}

type transactionTable struct {
	mu   sync.Mutex
	open map[uint64]*openTransaction
}

func newTransactionTable() *transactionTable {
	return &transactionTable{open: map[uint64]*openTransaction{}}
}

// newHandle mints an unguessable identifier.
//
// Random rather than sequential because a counter is guessable, and until the
// bus carries caller identity everywhere a guessable handle is a way to drive
// somebody else's transaction. The principal check below is the real control;
// this closes the gap where a handle leaks or is guessed before that check
// runs. Reading fewer than eight bytes of randomness is not recoverable -- a
// predictable handle is worse than no transaction -- so it refuses instead.
func newHandle() (uint64, error) {
	var raw [8]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return 0, err
	}
	value := binary.LittleEndian.Uint64(raw[:])
	if value == 0 {
		// Zero means "no transaction" on the wire, so it cannot also be one.
		value = 1
	}
	return value, nil
}

func (t *transactionTable) put(handle uint64, entry *openTransaction) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.open[handle] = entry
}

// countFor reports how many transactions a principal already holds.
func (t *transactionTable) countFor(principal uint32) int {
	t.mu.Lock()
	defer t.mu.Unlock()
	total := 0
	for _, entry := range t.open {
		if entry.principal == principal {
			total++
		}
	}
	return total
}

// claim finds a transaction for the caller that owns it, refusing one owned by
// anybody else. The two failures are deliberately indistinguishable to the
// caller -- both answer "no such transaction" -- because telling a caller that
// a handle exists but belongs to someone else confirms the handle is real.
func (t *transactionTable) claim(handle uint64, principal, srcHandle uint32) (*openTransaction, bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	entry, ok := t.open[handle]
	if !ok || entry.principal != principal || entry.handle != srcHandle {
		return nil, false
	}
	entry.lastUsed = time.Now()
	return entry, true
}

func (t *transactionTable) remove(handle uint64) *openTransaction {
	t.mu.Lock()
	defer t.mu.Unlock()
	entry := t.open[handle]
	delete(t.open, handle)
	return entry
}

// reapIdle rolls back and releases every transaction that has gone quiet.
//
// Rolled back rather than committed: a caller that stopped talking never said
// its work was complete, and committing a partial transaction on its behalf
// invents an intent nobody expressed.
func (t *transactionTable) reapIdle(ctx context.Context, now time.Time) int {
	t.mu.Lock()
	var expired []*openTransaction
	for handle, entry := range t.open {
		if now.Sub(entry.lastUsed) > transactionIdleTimeout {
			expired = append(expired, entry)
			delete(t.open, handle)
		}
	}
	t.mu.Unlock()

	for _, entry := range expired {
		_ = entry.tx.Rollback(ctx)
		entry.conn.Release()
	}
	return len(expired)
}
