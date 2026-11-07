package memory

import (
	"context"
	"sync/atomic"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// Hold initialization while the request-local datastore is copied, just as the
// personal index worker can overlap a foreground memory request in production.
type blockedCodeInitialization struct {
	store.Store
	entered chan struct{}
	release chan struct{}
	reads   atomic.Int32
}

func (d *blockedCodeInitialization) CurrentSchemaVersion(ctx context.Context, owner string) (int64, string, error) {
	d.reads.Add(1)
	select {
	case d.entered <- struct{}{}:
	default:
	}
	select {
	case <-ctx.Done():
		return 0, "", ctx.Err()
	case <-d.release:
		return 2, store.StoreChecksum(codeGenerationSchema), nil
	}
}

func TestRequestCopySharesCodeInitialization(t *testing.T) {
	db := &blockedCodeInitialization{entered: make(chan struct{}, 1), release: make(chan struct{})}
	data, err := NewPostgresDataStore(db, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	backend := data.(*postgresDataStore)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	initialized := make(chan error, 1)
	go func() { initialized <- backend.ensureCodeIndex(ctx) }()
	select {
	case <-db.entered:
	case <-ctx.Done():
		t.Fatal("initializer did not enter")
	}
	bound := *backend
	close(db.release)
	if err := <-initialized; err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- bound.ensureCodeIndex(ctx) }()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("request retained a copied locked initialization mutex")
	}
	// The actual dispatcher makes further request/actor/audit-bound copies. It
	// must reuse the published initialization and reach ordinary route validation.
	_, status := handleDataAttempt(handlerOptions{data: backend, placement: PlacementServer}, ring.ModuleInvocation{}, DataRequest{Operation: "code-index", CodeIndex: &CodeIndexRequest{Route: "://invalid"}}, nil)
	if status != ring.ModuleStatusInternal {
		t.Fatalf("invalid code route status=%v", status)
	}
	if n := db.reads.Load(); n != 1 {
		t.Fatalf("request copies reran completed initialization: %d", n)
	}
}
