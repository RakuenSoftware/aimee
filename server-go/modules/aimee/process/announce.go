package process

import (
	"context"
	"encoding/json"
	"log"
	"sync"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// Change notifications this module publishes (ouroboros ring D14), carved from
// principal ref 30 after its 23 stages; declared in process-contracts.json.
const (
	// KindWorkItemChanged follows a committed lifecycle write: the workflow
	// engine dispatches on it instead of sweeping every second.
	KindWorkItemChanged uint32 = 4096 + 30*256 + 24
	// KindDelegationChanged follows a committed delegation or agent-work
	// write: the core's delegate monitor reacts to it instead of a 30s sweep.
	KindDelegationChanged uint32 = 4096 + 30*256 + 25
)

// Announcer publishes this module's change notifications once it has a slot to
// publish on. Announcements are coalesced per kind: a consumer re-reads what it
// needs, so it needs to know that something changed, not how many times.
type Announcer struct {
	mu        sync.Mutex
	publisher ring.Publisher
	pending   map[uint32]string
	wake      chan struct{}
}

func NewAnnouncer() *Announcer {
	return &Announcer{pending: map[uint32]string{}, wake: make(chan struct{}, 1)}
}

// Announce records that kind changed, through op. It never blocks: it runs on
// a request's goroutine right after its transaction committed.
func (a *Announcer) Announce(kind uint32, op string) {
	a.mu.Lock()
	a.pending[kind] = op
	a.mu.Unlock()
	select {
	case a.wake <- struct{}{}:
	default:
	}
}

// Start publishes pending announcements on publisher until ctx ends. Called
// from the runtime's Started hook. An announcement made before the slot was
// attached is published now; a consumer also reconciles when it attaches.
func (a *Announcer) Start(ctx context.Context, publisher ring.Publisher) {
	a.mu.Lock()
	a.publisher = publisher
	a.mu.Unlock()
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case <-a.wake:
			}
			a.mu.Lock()
			pending := a.pending
			a.pending = map[uint32]string{}
			a.mu.Unlock()
			for kind, op := range pending {
				payload, _ := json.Marshal(map[string]string{"op": op})
				if err := publisher.Publish(ctx, kind, payload); err != nil && ctx.Err() == nil {
					log.Printf("aimee: announce kind %d: %v", kind, err)
				}
			}
		}
	}()
}
