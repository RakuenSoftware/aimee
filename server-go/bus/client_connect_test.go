package bus

import (
	"context"
	"sync"
	"testing"
	"time"
)

// recordingHeartbeat counts how often a client's liveness was advanced.
type recordingHeartbeat struct {
	mu   sync.Mutex
	last uint64
	n    int
}

func (r *recordingHeartbeat) Heartbeat(now uint64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.last = now
	r.n++
}

func (r *recordingHeartbeat) count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.n
}

// A caller is idle between calls, unlike a serving module whose poll loop
// heartbeats every pass. Without this the host reaped the slot 30s after
// attach and silently dropped every later request -- the caller then waited out
// its whole deadline for a reply to a request nobody ever received.
func TestConnectedClientKeepsItsSlotAliveWhileIdle(t *testing.T) {
	rec := &recordingHeartbeat{}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	keepAlive(ctx, rec, time.Millisecond)

	deadline := time.Now().Add(2 * time.Second)
	for rec.count() < 3 && time.Now().Before(deadline) {
		time.Sleep(2 * time.Millisecond)
	}
	if got := rec.count(); got < 3 {
		t.Fatalf("idle client heartbeat advanced %d times; the host would reap its slot", got)
	}

	// Ending the context must stop it: a heartbeat outliving its owner would
	// keep a dead client's slot admitted.
	cancel()
	time.Sleep(20 * time.Millisecond)
	settled := rec.count()
	time.Sleep(50 * time.Millisecond)
	if after := rec.count(); after != settled {
		t.Fatalf("heartbeat kept running after ctx ended: %d -> %d", settled, after)
	}
}
