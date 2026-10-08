package backendstore

import (
	"context"
	"encoding/json"
	"errors"
	memory "github.com/JBailes/aimee/server-go/memory"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

const testOwner = "11111111-1111-4111-8111-111111111111"

func TestDurableBackendConformance(t *testing.T) {
	ctx := context.Background()
	scope := memory.Scope{Type: memory.ScopeProject, Value: "conversation"}
	foreign := memory.Scope{Type: memory.ScopeProject, Value: "other"}
	dir := filepath.Join(t.TempDir(), "cognee")
	s, err := New(dir, testOwner)
	if err != nil {
		t.Fatal(err)
	}
	author := json.RawMessage(`{"principal":"Virant","category":"user_stated","session_id":"conversation-1"}`)
	first, _, err := s.Mutate(ctx, Mutation{Scope: scope, Record: memory.Record{Kind: "fact", Key: "height", Content: "69cm", Confidence: 1, Authorship: author}, Key: "create-1", Digest: "first"})
	if err != nil {
		t.Fatal(err)
	}
	replay, _, err := s.Mutate(ctx, Mutation{Scope: scope, Record: memory.Record{}, Key: "create-1", Digest: "first"})
	if err != nil || replay.ID != first.ID {
		t.Fatal(replay, err)
	}
	if _, _, err = s.Mutate(ctx, Mutation{Scope: scope, Key: "create-1", Digest: "different"}); !errors.Is(err, ErrIdempotency) {
		t.Fatal(err)
	}
	s, err = New(dir, testOwner)
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Get(ctx, scope, first.ID)
	if err != nil || got.Content != "69cm" || string(got.Authorship) != string(author) {
		t.Fatal(got, err)
	}
	if _, err = s.Get(ctx, foreign, first.ID); !errors.Is(err, memory.ErrNotFound) {
		t.Fatal("foreign scope", err)
	}
	correction := got
	correction.Content = "170cm"
	current, _, err := s.Mutate(ctx, Mutation{Scope: scope, Record: correction, Expected: first.Version})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err = s.Mutate(ctx, Mutation{Scope: scope, Record: correction, Expected: first.Version}); !errors.Is(err, ErrConflict) {
		t.Fatal("stale mutation accepted", err)
	}
	if _, _, err = s.Mutate(ctx, Mutation{Scope: scope, Key: "create-1", Digest: "first"}); !errors.Is(err, ErrReplayUnavailable) {
		t.Fatal("stale creation replay", err)
	}
	old, err := s.GetVersion(ctx, scope, first.ID, first.Version)
	if err != nil || old.Content != "69cm" || !old.Historical {
		t.Fatal(old, err)
	}
	if current.Version.RecordRevision == first.Version.RecordRevision {
		t.Fatal("revision not advanced")
	}
	snapshot, err := s.Export(ctx)
	if err != nil {
		t.Fatal(err)
	}
	target, err := New(filepath.Join(t.TempDir(), "hillock"), testOwner)
	if err != nil {
		t.Fatal(err)
	}
	if err = target.Import(ctx, snapshot); err != nil {
		t.Fatal(err)
	}
	migrated, err := target.Get(ctx, scope, first.ID)
	if err != nil || *migrated.Version != *current.Version || migrated.Content != "170cm" {
		t.Fatal(migrated, err)
	}
	if _, _, err = target.Mutate(ctx, Mutation{Scope: scope, Record: memory.Record{ID: first.ID}, Delete: true, Expected: current.Version, Key: "delete-1", Digest: "deletion"}); err != nil {
		t.Fatal(err)
	}
	if _, deleted, err := target.Mutate(ctx, Mutation{Scope: scope, Key: "delete-1", Digest: "deletion"}); err != nil || !deleted {
		t.Fatal("delete retry", deleted, err)
	}
	if _, err = target.Get(ctx, scope, first.ID); !errors.Is(err, memory.ErrNotFound) {
		t.Fatal(err)
	}
	if historical, err := target.GetVersion(ctx, scope, first.ID, current.Version); err != nil || !historical.Historical {
		t.Fatal(historical, err)
	}
	// Physical restore of an older catalog must replay separately retained intent.
	if _, err = s.EraseSubject(ctx, "Virant", nil); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(dir, "records.json"), snapshot, 0600); err != nil {
		t.Fatal(err)
	}
	s, err = New(dir, testOwner)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = s.Get(ctx, scope, first.ID); !errors.Is(err, memory.ErrNotFound) {
		t.Fatal("restore resurrected erased source", err)
	}
	if _, err = s.GetVersion(ctx, scope, first.ID, first.Version); !errors.Is(err, memory.ErrNotFound) {
		t.Fatal("history retained erased source", err)
	}
	if err = s.Import(ctx, snapshot); !errors.Is(err, ErrConflict) {
		t.Fatal("import resurrected erased source", err)
	}
	if _, err = s.Put(ctx, scope, correction); err == nil {
		t.Fatal("late write resurrected erased source")
	}
	raw, err := s.Export(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var state Snapshot
	if err = json.Unmarshal(raw, &state); err != nil || len(state.Entries) != 0 || len(state.Receipts) != 0 {
		t.Fatal("erased payloads retained", err)
	}
}
func TestDurableBackendConcurrentWritersAndPaging(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "catalog")
	ctx := context.Background()
	scope := memory.Scope{Type: memory.ScopeGlobal, Value: "_global"}
	a, err := New(dir, testOwner)
	if err != nil {
		t.Fatal(err)
	}
	b, err := New(dir, testOwner)
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			store := a
			if i%2 != 0 {
				store = b
			}
			key := string(rune('a' + i))
			if _, err := store.Put(ctx, scope, memory.Record{Kind: "fact", Key: key, Content: key}); err != nil {
				t.Error(err)
			}
		}(i)
	}
	wg.Wait()
	seen := map[int64]bool{}
	after := int64(0)
	for {
		page, err := a.List(ctx, scope, after, 3)
		if err != nil {
			t.Fatal(err)
		}
		if len(page) == 0 {
			break
		}
		for _, r := range page {
			if seen[r.ID] {
				t.Fatal("duplicate ID")
			}
			seen[r.ID] = true
			after = r.ID
		}
	}
	if len(seen) != 20 {
		t.Fatal("lost concurrent writes", len(seen))
	}
	canceled, cancel := context.WithCancel(ctx)
	cancel()
	if _, err = a.Put(canceled, scope, memory.Record{Kind: "fact", Key: "canceled"}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
}

func TestImportRejectsMalformedHistory(t *testing.T) {
	ctx := context.Background()
	source, err := New(filepath.Join(t.TempDir(), "source"), testOwner)
	if err != nil {
		t.Fatal(err)
	}
	scope := memory.Scope{Type: "project", Value: "fixture"}
	first, err := source.Put(ctx, scope, memory.Record{Kind: "fact", Key: "height", Content: "69cm", Confidence: 1})
	if err != nil {
		t.Fatal(err)
	}
	first.Content = "170cm"
	if _, err = source.Put(ctx, scope, first); err != nil {
		t.Fatal(err)
	}
	raw, err := source.Export(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var snapshot Snapshot
	if err = json.Unmarshal(raw, &snapshot); err != nil {
		t.Fatal(err)
	}
	entry := snapshot.Entries[first.ID]
	entry.History[0].Version = nil
	snapshot.Entries[first.ID] = entry
	raw, _ = json.Marshal(snapshot)
	destination, err := New(filepath.Join(t.TempDir(), "destination"), testOwner)
	if err != nil {
		t.Fatal(err)
	}
	if err = destination.Import(ctx, raw); !errors.Is(err, memory.ErrClientRequest) {
		t.Fatal("malformed history accepted", err)
	}
}
