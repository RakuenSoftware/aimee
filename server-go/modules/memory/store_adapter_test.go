package memory

import (
	"context"
	"errors"
	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
	"testing"
	"time"
)

func TestMemoryStoreContractUsesExistingDataAPI(t *testing.T) {
	native := &recordingDataStore{}
	handler := NewHandler(nil, WithDataStore(PlacementKB, native))
	client, err := contract.NewClient(clientCallFunc(func(_ context.Context, event, stage uint32, trace uint64, _ time.Duration, raw []byte) ([]byte, error) {
		if event != EventData || stage != StageData || trace != 73 {
			t.Fatal("existing routing changed")
		}
		reply, status := handler(bus.ModuleInvocation{StageID: stage, TraceID: trace, PrincipalRef: 0}, raw)
		if status != bus.ModuleStatusOK {
			return nil, &bus.ModuleCallStatusError{Status: status}
		}
		return reply, nil
	}), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	var backend contract.Backend = contract.ClientStore{Client: client, TraceID: 73}
	scope := Scope{Type: ScopeProject, Value: "aimee"}
	ctx := context.Background()
	got, err := backend.Get(ctx, scope, 42)
	if err != nil || got.ID != 42 || got.Scope != scope {
		t.Fatal(got, err)
	}
	got, err = backend.Put(ctx, scope, contract.Record{Key: "contract", Content: "unchanged native storage", Kind: "fact", Tier: "L2", Confidence: .9})
	if err != nil || got.Key != "contract" || got.Scope != scope {
		t.Fatal(got, err)
	}
	records, err := backend.Search(ctx, scope, "contract", "fact", "L2", 4)
	if err != nil || len(records) != 0 || native.scope != scope {
		t.Fatal(records, err)
	}
	deleted, err := backend.Delete(ctx, scope, 42)
	if err != nil || !deleted {
		t.Fatal(deleted, err)
	}
	if backend.Capabilities().Version != 1 {
		t.Fatal("missing contract version")
	}
	var empty *contract.Client
	_, err = (contract.ClientStore{Client: empty}).Get(ctx, scope, 42)
	if !errors.Is(err, contract.ErrClientConfig) {
		t.Fatal("nil client contract changed", err)
	}
}
func TestNativeStorePreservesContractRecordFields(t *testing.T) {
	source := Record{ID: 9007199254740993, Scope: Scope{Type: ScopeUser, Value: "_user"}, Key: "identity", Kind: "fact", Tier: "L2", Content: "canonical", Confidence: .9, observedVersion: &MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-4000-8000-000000000001", RecordID: "9007199254740993", RecordRevision: "2"}}
	exposed, err := toContractRecord(source)
	if err != nil || exposed.ID != source.ID || exposed.Version == nil || *exposed.Version != *source.observedVersion {
		t.Fatal(exposed, err)
	}
	back, err := fromContractRecord(exposed)
	if err != nil || back.ID != source.ID || back.Scope != source.Scope || back.Content != source.Content {
		t.Fatal(back, err)
	}
}

// A failed provider cleanup must stop the existing admitted lifecycle path
// before it reaches canonical SQL. A nil Queryer makes an accidental write fail.
type failingDerivedBackend struct {
	NativeStore
	called bool
}

func (b *failingDerivedBackend) Forget(context.Context, Scope, int64) error {
	b.called = true
	return contract.ErrUnavailable
}
func TestMemoryBackendDeletionStopsBeforeCanonicalWrite(t *testing.T) {
	provider := &failingDerivedBackend{}
	native := &postgresDataStore{backendFactory: func(source contract.Store) (contract.Backend, error) {
		bound, ok := source.(NativeStore)
		if !ok || bound.Data.(*postgresDataStore).backendFactory != nil {
			t.Fatal("provider source recursively dispatches to provider")
		}
		return provider, nil
	}}
	receipt, err := native.applyKBDeletion(context.Background(), kbDeletion{id: 42, scope: Scope{Type: ScopeProject, Value: "aimee"}, authority: AuthorityUser})
	if receipt != nil || !errors.Is(err, contract.ErrUnavailable) || !provider.called {
		t.Fatal("canonical deletion continued after failed cleanup", receipt, err)
	}
}

func TestMemoryStoreContractCanonicalScopes(t *testing.T) {
	for _, item := range []struct {
		placement Placement
		scope     Scope
		value     string
	}{
		{PlacementServer, Scope{Type: ScopeUser}, "_user"},
		{PlacementKB, Scope{Type: ScopeGlobal}, "_global"},
	} {
		handler := NewHandler(nil, WithDataStore(item.placement, &recordingDataStore{}))
		client, err := contract.NewClient(clientCallFunc(func(_ context.Context, event, stage uint32, trace uint64, _ time.Duration, raw []byte) ([]byte, error) {
			reply, status := handler(bus.ModuleInvocation{StageID: stage}, raw)
			if status != bus.ModuleStatusOK {
				t.Fatal(status)
			}
			return reply, nil
		}), time.Second)
		if err != nil {
			t.Fatal(err)
		}
		got, err := (contract.ClientStore{Client: client}).Get(context.Background(), item.scope, 42)
		if err != nil || got.Scope.Value != item.value {
			t.Fatal(got, err)
		}
	}
}

func TestMemoryBackendConfiguration(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_BACKEND", "native")
	if factory, err := configuredMemoryBackend(nil); err != nil || factory != nil {
		t.Fatal("native default changed", err)
	}
	t.Setenv("AIMEE_MEMORY_BACKEND", "unregistered")
	if _, err := configuredMemoryBackend(nil); err == nil {
		t.Fatal("unknown provider silently accepted")
	}
	home := t.TempDir()
	if _, err := identity.Ensure(home, "server"); err != nil {
		t.Fatal(err)
	}
	t.Setenv("AIMEE_HOME", home)
	t.Setenv("AIMEE_MEMORY_BACKEND", "cognee")
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", "https://cognee.example")
	t.Setenv("AIMEE_MEMORY_BACKEND_AUTH", "bearer")
	factory, err := configuredMemoryBackend(nil)
	if err != nil || factory == nil {
		t.Fatal(err)
	}
	provider, err := factory(NativeStore{Data: &recordingDataStore{}})
	if err != nil || provider.Capabilities().Name != "cognee" {
		t.Fatal(provider, err)
	}
	// Exact reads still use the injected generic source; no network or native
	// SQL is required by the Cognee implementation for a canonical read.
	scope := Scope{Type: ScopeProject, Value: "contract"}
	record, err := provider.Get(context.Background(), scope, 42)
	if err != nil || record.ID != 42 || record.Scope != scope {
		t.Fatal(record, err)
	}
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", "https://cognee.example/admin")
	if _, err := configuredMemoryBackend(nil); err == nil {
		t.Fatal("invalid origin accepted")
	}
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", "https://cognee.example")
	t.Setenv("AIMEE_MEMORY_BACKEND_AUTH", "unknown")
	if _, err := configuredMemoryBackend(nil); err == nil {
		t.Fatal("unknown auth accepted")
	}
}
