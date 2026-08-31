package bus

import (
	"context"
	"sync/atomic"
	"testing"
	"time"
)

// CloseAndWait must not return while the poll goroutine is still reading.
//
// WHY THIS TEST IS INDIRECT. The defect it guards is a segfault: poll reads the
// bus's shared-memory region and Detach unmaps it, so detaching while the
// goroutine is live is a read through an unmapped page. No in-process test can
// reproduce that -- a fake bus has no region to unmap -- which is why it
// survived twenty-odd green runs in a peer module and was only found on real
// hardware, as a fault AFTER the last assertion.
//
// So this pins the property that makes the correct ordering possible: that
// CloseAndWait observes the goroutine's exit rather than merely requesting it.
//
// THE FIRST VERSION OF THIS TEST DID NOT WORK. It closed a channel and slept,
// and with the wait removed it still passed, because the loop notices the stop
// signal and returns faster than the sleep. A test for a race has to hold the
// race open. This one blocks the fake bus INSIDE Poll, so the goroutine is
// provably mid-read when the stop is requested: without the wait, CloseAndWait
// returns while that read is still in flight, and the assertion catches it.

// blockingBus parks inside Poll until released, so the poll goroutine can be
// held in a read for as long as the test needs.
type blockingBus struct {
	entered  chan struct{}
	release  chan struct{}
	inPoll   atomic.Bool
	released atomic.Bool
}

func (b *blockingBus) Poll() (Event, bool, error) {
	b.inPoll.Store(true)
	defer b.inPoll.Store(false)
	select {
	case b.entered <- struct{}{}:
	default:
	}
	<-b.release
	return Event{}, false, nil
}

func (b *blockingBus) RequestFragment(uint32, uint64, []byte, bool) error { return nil }
func (b *blockingBus) Cancel(uint32, uint64) error                        { return nil }
func (b *blockingBus) moduleInlineBudget() uint32                         { return 1024 }

func TestCloseAndWaitDoesNotReturnWhileThePollLoopIsReading(t *testing.T) {
	b := &blockingBus{entered: make(chan struct{}, 1), release: make(chan struct{})}
	c := newConcurrentModuleCaller(context.Background(), b)

	// Wait until the goroutine is genuinely inside a read, so the test is about
	// the race rather than about timing.
	select {
	case <-b.entered:
	case <-time.After(2 * time.Second):
		t.Fatal("the poll loop never entered Poll; this test would prove nothing")
	}
	if !b.inPoll.Load() {
		t.Fatal("the poll loop is not inside Poll; the race is not held open")
	}

	returned := make(chan struct{})
	go func() {
		c.CloseAndWait()
		close(returned)
	}()

	// CloseAndWait must still be blocked: the goroutine is parked in a read it
	// has not finished. This is the assertion the missing wait fails.
	select {
	case <-returned:
		t.Fatal("CloseAndWait returned while the poll goroutine was still inside " +
			"a read: detaching here unmaps the region under it")
	case <-time.After(200 * time.Millisecond):
	}

	// Let the read finish; now the wait may return.
	b.released.Store(true)
	close(b.release)
	select {
	case <-returned:
	case <-time.After(3 * time.Second):
		t.Fatal("CloseAndWait never returned after the poll loop was released")
	}
	if b.inPoll.Load() {
		t.Error("CloseAndWait returned while a read was still in flight")
	}
}

// A second call must not block or panic: shutdown is reached from more than one
// path and none of them should have to coordinate with the others.
func TestCloseAndWaitIsSafeTwice(t *testing.T) {
	b := &blockingBus{entered: make(chan struct{}, 1), release: make(chan struct{})}
	close(b.release) // never park; this test is about the second call
	c := newConcurrentModuleCaller(context.Background(), b)

	done := make(chan struct{})
	go func() {
		c.CloseAndWait()
		c.CloseAndWait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("a second CloseAndWait blocked")
	}
}
