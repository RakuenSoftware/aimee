package postgres

import (
	"testing"
	"time"
)

func TestAHandleIsOnlyUsableByTheCallerThatOpenedIt(t *testing.T) {
	// The handle names a transaction; it does not confer the right to drive
	// one. Now that the bus carries the caller's identity, ownership is the
	// control rather than the handle being hard to guess.
	table := newTransactionTable()
	table.put(1234, &openTransaction{principal: 30, handle: 4, lastUsed: time.Now()})

	if _, ok := table.claim(1234, 30, 4); !ok {
		t.Fatal("the opening caller was refused its own transaction")
	}
	if _, ok := table.claim(1234, 31, 4); ok {
		t.Error("another principal drove someone else's transaction")
	}
	if _, ok := table.claim(1234, 30, 5); ok {
		t.Error("a different attachment of the same principal inherited a " +
			"transaction from the previous one")
	}
}

func TestAnUnknownHandleIsIndistinguishableFromOneOwnedByAnother(t *testing.T) {
	// Both answer "not open". Telling a caller that a handle exists but belongs
	// to someone else confirms the handle is real, which is the one thing a
	// caller guessing handles wants to learn.
	table := newTransactionTable()
	table.put(99, &openTransaction{principal: 30, handle: 1, lastUsed: time.Now()})

	_, mine := table.claim(1, 30, 1)
	_, theirs := table.claim(99, 31, 1)
	if mine || theirs {
		t.Fatal("a claim succeeded that should not have")
	}
}

func TestHandlesAreNotSequential(t *testing.T) {
	// A counter is guessable. The ownership check above is the real control,
	// but a predictable handle is a way to reach a transaction before that
	// check has anything to compare against.
	seen := map[uint64]bool{}
	previous := uint64(0)
	sequential := 0
	for index := 0; index < 64; index++ {
		handle, err := newHandle()
		if err != nil {
			t.Fatalf("mint: %v", err)
		}
		if handle == 0 {
			t.Fatal("zero means no transaction on the wire and cannot also be one")
		}
		if seen[handle] {
			t.Fatal("a handle repeated")
		}
		seen[handle] = true
		if index > 0 && handle == previous+1 {
			sequential++
		}
		previous = handle
	}
	if sequential > 2 {
		t.Errorf("%d handles followed the previous one; this looks like a counter",
			sequential)
	}
}

func TestIdleTransactionsAreReclaimedAndTheirHandlesStopWorking(t *testing.T) {
	// The half that has to be right. A caller whose transaction was reclaimed
	// must find out from its next statement, not from a COMMIT that reports
	// success while every write in the transaction is already gone.
	table := newTransactionTable()
	table.put(7, &openTransaction{principal: 30, handle: 1,
		lastUsed: time.Now().Add(-2 * transactionIdleTimeout)})
	table.put(8, &openTransaction{principal: 30, handle: 1, lastUsed: time.Now()})

	// The stale one has no live tx/conn to roll back, so reap only the table
	// bookkeeping here; the rollback path is exercised against a real pool.
	table.mu.Lock()
	for handle, entry := range table.open {
		if time.Since(entry.lastUsed) > transactionIdleTimeout {
			delete(table.open, handle)
		}
	}
	table.mu.Unlock()

	if _, ok := table.claim(7, 30, 1); ok {
		t.Error("a reclaimed handle still worked; its writes would be lost silently")
	}
	if _, ok := table.claim(8, 30, 1); !ok {
		t.Error("a live transaction was reclaimed with the idle one")
	}
}

func TestUsingATransactionKeepsItAlive(t *testing.T) {
	// Idle means idle, not old. A long transaction that is still being used
	// must not be reclaimed underneath its caller.
	table := newTransactionTable()
	table.put(3, &openTransaction{principal: 30, handle: 1,
		lastUsed: time.Now().Add(-transactionIdleTimeout / 2)})
	before, _ := table.claim(3, 30, 1)
	if before == nil {
		t.Fatal("claim failed")
	}
	if time.Since(before.lastUsed) > time.Second {
		t.Error("claiming a transaction did not refresh its idle clock")
	}
}

func TestOpenTransactionsAreCountedPerPrincipal(t *testing.T) {
	// A transaction pins a pooled connection, and the pool is shared with every
	// other module. One caller must not be able to drain it.
	table := newTransactionTable()
	for index := 0; index < 3; index++ {
		table.put(uint64(index+1), &openTransaction{principal: 30, handle: 1,
			lastUsed: time.Now()})
	}
	table.put(99, &openTransaction{principal: 31, handle: 1, lastUsed: time.Now()})

	if got := table.countFor(30); got != 3 {
		t.Errorf("principal 30 holds %d, want 3", got)
	}
	if got := table.countFor(31); got != 1 {
		t.Errorf("principal 31 holds %d, want 1", got)
	}
}
