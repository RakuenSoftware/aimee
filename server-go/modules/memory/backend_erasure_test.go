//go:build !windows

package memory

import (
	"context"
	"errors"
	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/memory"
	"golang.org/x/sys/unix"
	"sync/atomic"
	"testing"
	"time"
)

type resetFixtureStore struct {
	NativeStore
	entered, release chan struct{}
	copies           atomic.Bool
	resetCalls       atomic.Int32
	fail             atomic.Bool
}

func (s *resetFixtureStore) Search(ctx context.Context, scope Scope, q, k, t string, limit int) ([]contract.Record, error) {
	close(s.entered)
	<-s.release // An already-started index operation can outlive cancellation.
	s.copies.Store(true)
	return nil, nil
}
func (s *resetFixtureStore) ResetDerived(context.Context) error {
	s.resetCalls.Add(1)
	if s.fail.Load() {
		return contract.ErrUnavailable
	}
	s.copies.Store(false)
	return nil
}
func (s *resetFixtureStore) Forget(context.Context, Scope, int64) error { return nil }

func TestMemorySubjectErasureResetAdmissionAndRetry(t *testing.T) {
	provider := &resetFixtureStore{}
	handler := NewHandler(nil, WithDataStore(PlacementKB, ContractDataStore{Store: provider}))
	raw := []byte(`{"operation":"reset-derived"}`)
	if _, status := handler(bus.ModuleInvocation{StageID: StageData, PrincipalRef: 73}, raw); status != bus.ModuleStatusCapabilityAbsent || provider.resetCalls.Load() != 0 {
		t.Fatal("non-host reset admitted", status)
	}
	provider.fail.Store(true)
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, raw); status != bus.ModuleStatusInternal {
		t.Fatal("unknown cleanup acknowledged", status)
	}
	provider.fail.Store(false)
	client, err := contract.NewClient(clientCallFunc(func(ctx context.Context, event, stage uint32, trace uint64, _ time.Duration, body []byte) ([]byte, error) {
		if event != EventData || stage != StageData || trace != 22 {
			t.Fatal("changed lifecycle routing")
		}
		reply, status := handler(bus.ModuleInvocation{StageID: stage}, body)
		if status != bus.ModuleStatusOK {
			return nil, errors.New("reset failed")
		}
		return reply, nil
	}), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if err := client.ResetDerived(context.Background(), 22); err != nil {
		t.Fatal("retry failed", err)
	}
	if err := client.ResetDerived(context.Background(), 22); err != nil {
		t.Fatal("idempotent retry failed", err)
	}
	// A provider advertising derived storage must supply the reset extension;
	// missing support cannot become successful erasure coverage.
	missing := &postgresDataStore{backendFactory: func(contract.Store) (contract.Backend, error) { return &failingDerivedBackend{}, nil }}
	if err := resetDerivedBackend(context.Background(), missing); !errors.Is(err, contract.ErrUnsupported) {
		t.Fatal("missing reset falsely acknowledged", err)
	}
	// The native provider requires no new SQL, remote service or coordinator.
	native := NewHandler(nil, WithDataStore(PlacementServer, &postgresDataStore{}))
	if _, status := native(bus.ModuleInvocation{StageID: StageData}, raw); status != bus.ModuleStatusOK {
		t.Fatal("native cleanup changed", status)
	}
}

func TestMemorySubjectErasureWaitsForInFlightIndexing(t *testing.T) {
	provider := &resetFixtureStore{entered: make(chan struct{}), release: make(chan struct{})}
	handler := NewHandler(nil, WithDataStore(PlacementKB, ContractDataStore{Store: provider}))
	finished := make(chan bus.ModuleStatus, 1)
	go func() {
		_, status := handler(bus.ModuleInvocation{StageID: StageData}, []byte(`{"operation":"search","scope":{"type":"project","value":"aimee"},"query":"needle","limit":1}`))
		finished <- status
	}()
	select {
	case <-provider.entered:
	case <-time.After(time.Second):
		t.Fatal("indexing not entered")
	}
	var now unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &now); err != nil {
		t.Fatal(err)
	}
	inv := bus.ModuleInvocation{StageID: StageData, DeadlineNS: uint64(now.Nano() + int64(30*time.Millisecond))}
	if _, status := handler(inv, []byte(`{"operation":"reset-derived"}`)); status != bus.ModuleStatusCancelled || provider.resetCalls.Load() != 0 {
		t.Fatal("queued cancelled reset executed", status)
	}
	// Erasure has now committed in the source owner; the old index operation
	// finishes first. Cleanup must run afterward, and not report success before.
	close(provider.release)
	if status := <-finished; status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	if !provider.copies.Load() {
		t.Fatal("fixture did not reproduce an old index write")
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, []byte(`{"operation":"reset-derived"}`)); status != bus.ModuleStatusOK || provider.copies.Load() {
		t.Fatal("old indexing resurrected an erased copy", status)
	}
}
