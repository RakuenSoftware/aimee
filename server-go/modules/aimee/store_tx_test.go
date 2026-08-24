package aimee

import (
	"errors"
	"testing"
)

// A transaction handle can stop being valid without aimee doing anything: the
// store expires idle handles and reclaims them when a connection drops. When
// that happens the next statement has to say so in a way aimee can act on.
//
// The distinction that matters: a statement the store refused on its merits
// leaves the transaction live and one statement wrong, while a reclaimed handle
// means every write in the transaction is already gone and the whole operation
// has to start over. Collapsing the two would let a family log "insert failed,
// carrying on" and commit a transaction that no longer exists.
func TestAReclaimedTransactionIsNotAFailedStatement(t *testing.T) {
	for _, sqlstate := range []string{
		sqlStateNoActiveTransaction,
		sqlStateInFailedTransaction,
	} {
		t.Run(sqlstate, func(t *testing.T) {
			f := &fakeCaller{reply: refusal(sqlstate, "transaction handle is not open")}
			_, err := newStore(t, f).Exec(t.Context(), "INSERT INTO t VALUES (1)")
			if !errors.Is(err, ErrTxClosed) {
				t.Errorf("reclaimed handle = %v, want ErrTxClosed", err)
			}
		})
	}
}

// The converse: an ordinary refusal must NOT read as a lost transaction, or a
// caller would abandon and retry work that is still perfectly live.
func TestAnOrdinaryRefusalIsNotAReclaimedTransaction(t *testing.T) {
	f := &fakeCaller{reply: refusal("23505", "duplicate key value")}
	_, err := newStore(t, f).Exec(t.Context(), "INSERT INTO t VALUES (1)")
	if errors.Is(err, ErrTxClosed) {
		t.Error("a unique violation was reported as a closed transaction")
	}
	if !IsUniqueViolation(err) {
		t.Errorf("unique violation lost its classification: %v", err)
	}
}

// The SQLSTATE and message still reach a human reading the log: ErrTxClosed
// says what to do, the wrapped StoreError says what happened.
func TestAReclaimedTransactionKeepsItsDetail(t *testing.T) {
	f := &fakeCaller{reply: refusal(sqlStateNoActiveTransaction, "handle 0x9f expired after 30s idle")}
	_, err := newStore(t, f).Exec(t.Context(), "INSERT INTO t VALUES (1)")

	var se *StoreError
	if !errors.As(err, &se) {
		t.Fatalf("no StoreError inside %v", err)
	}
	if se.SQLState != sqlStateNoActiveTransaction {
		t.Errorf("SQLState = %q, want %q", se.SQLState, sqlStateNoActiveTransaction)
	}
	if se.Message == "" {
		t.Error("the store's explanation was dropped")
	}
}
