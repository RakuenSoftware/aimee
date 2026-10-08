package backendstore

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	memory "github.com/JBailes/aimee/server-go/memory"
	"os"
	"path/filepath"
	"testing"
)

func TestErasureEpochNewWritesAndStaleRestore(t *testing.T) {
	ctx := context.Background()
	scope := memory.Scope{Type: memory.ScopeUser, Value: "_user"}
	dir := filepath.Join(t.TempDir(), "catalog")
	s, err := New(dir, testOwner)
	if err != nil {
		t.Fatal(err)
	}
	record := memory.Record{Kind: "fact", Key: "height", Content: "69cm", Confidence: 1, Authorship: json.RawMessage(`{"principal":"Virant","erasure_epoch":"0"}`)}
	old, err := s.Put(ctx, scope, record)
	if err != nil {
		t.Fatal(err)
	}
	stale, _ := s.Export(ctx)
	if _, err = s.EraseSubject(ctx, "Virant", nil); err != nil {
		t.Fatal(err)
	}
	if _, err = s.Put(ctx, scope, record); !errors.Is(err, ErrConflict) {
		t.Fatal("old write admitted", err)
	}
	epoch, err := s.AdmissionEpoch(ctx, "Virant")
	if err != nil || epoch != 1 {
		t.Fatal(epoch, err)
	}
	record.Authorship = json.RawMessage(`{"principal":"Virant","erasure_epoch":"1"}`)
	fresh, err := s.Put(ctx, scope, record)
	if err != nil || fresh.ID == old.ID {
		t.Fatal(fresh, err)
	}
	backup, _ := s.Export(ctx)
	if _, err = s.EraseSubject(ctx, "Virant", nil); err != nil {
		t.Fatal(err)
	}
	for _, snapshot := range [][]byte{stale, backup} {
		if err = os.WriteFile(filepath.Join(dir, "records.json"), snapshot, 0600); err != nil {
			t.Fatal(err)
		}
		restored, e := New(dir, testOwner)
		if e != nil {
			t.Fatal(e)
		}
		if rows, e := restored.Search(ctx, scope, "", "", "", 10); e != nil || len(rows) != 0 {
			t.Fatal("resurrection", rows, e)
		}
		if epoch, e := restored.AdmissionEpoch(ctx, "Virant"); e != nil || epoch != 2 {
			t.Fatal("epoch rollback", epoch, e)
		}
	}
}

func TestCollectionRevalidationGuardsAndGrowth(t *testing.T) {
	ctx := context.Background()
	s, err := New(filepath.Join(t.TempDir(), "catalog"), testOwner)
	if err != nil {
		t.Fatal(err)
	}
	scope := memory.Scope{Type: memory.ScopeProject, Value: "chat"}
	for i := 0; i < 300; i++ {
		_, err = s.Put(ctx, scope, memory.Record{Kind: "fact", Key: fmt.Sprint(i), Content: "ordinary", Confidence: 1})
		if err != nil {
			t.Fatal(err)
		}
	}
	needle, err := s.Put(ctx, scope, memory.Record{Kind: "fact", Key: "Kibukx", Content: "height 69cm", Confidence: 1})
	if err != nil {
		t.Fatal(err)
	}
	rows, err := s.Candidates(ctx, scope, "Kibukx height", "", "", 256)
	if err != nil || len(rows) != 256 || rows[0].ID != needle.ID {
		t.Fatal(rows, err)
	}
	version, err := s.Observe(ctx)
	if err != nil {
		t.Fatal(err)
	}
	check := func(guard string) bool {
		t.Helper()
		ok, e := s.Revalidate(ctx, "check", guard, &version, []memory.Scope{scope}, []memory.MemoryRecordVersion{*needle.Version})
		if e != nil {
			t.Fatal(e)
		}
		return ok
	}
	if !check("acquire") || !check("") {
		t.Fatal("acquire changed collection")
	}
	second, e := New(s.dir, testOwner)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = second.EraseSubject(ctx, "Virant", nil); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal("erasure bypassed guard", e)
	}
	if _, e = second.Put(ctx, scope, memory.Record{Kind: "fact", Key: "blocked", Content: "test"}); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal("write bypassed guard", e)
	}
	if !check("release") {
		t.Fatal("release")
	}
	if _, e = second.Put(ctx, scope, memory.Record{Kind: "fact", Key: "new", Content: "test"}); e != nil {
		t.Fatal(e)
	}
	if check("") {
		t.Fatal("stale collection admitted")
	}
}

func TestLegacyErasureAdmissionUpgradeAndRetry(t *testing.T) {
	ctx := context.Background()
	dir := filepath.Join(t.TempDir(), "legacy")
	s, e := New(dir, testOwner)
	if e != nil {
		t.Fatal(e)
	}
	raw, _ := json.Marshal(Snapshot{Erased: map[int64]bool{}, ErasedSubjects: map[string]bool{erasureMarker("Virant"): true}})
	if e = os.WriteFile(filepath.Join(dir, "erasures.json"), raw, 0600); e != nil {
		t.Fatal(e)
	}
	s, e = New(dir, testOwner)
	if e != nil {
		t.Fatal(e)
	}
	epoch, e := s.AdmissionEpoch(ctx, "Virant")
	if e != nil || epoch != 1 {
		t.Fatal(epoch, e)
	}
	scope := memory.Scope{Type: memory.ScopeUser, Value: "_user"}
	record := memory.Record{Kind: "fact", Key: "fresh", Content: "new", Authorship: json.RawMessage(`{"principal":"Virant","erasure_epoch":"1"}`)}
	if _, e = s.Put(ctx, scope, record); e != nil {
		t.Fatal(e)
	}
	if _, e = s.EraseSubjectRequest(ctx, "Virant", nil, "erase-operation-1"); e != nil {
		t.Fatal(e)
	}
	record.Authorship = json.RawMessage(`{"principal":"Virant","erasure_epoch":"2"}`)
	fresh, e := s.Put(ctx, scope, record)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = s.EraseSubjectRequest(ctx, "Virant", nil, "erase-operation-1"); e != nil {
		t.Fatal(e)
	}
	if _, e = s.Get(ctx, scope, fresh.ID); e != nil {
		t.Fatal("erasure retry removed fresh admission", e)
	}
	if _, e = s.EraseSubjectRequest(ctx, "different", nil, "erase-operation-1"); !errors.Is(e, ErrIdempotency) {
		t.Fatal("erasure identity rebound", e)
	}
}

func TestExpiredSendLeaseRetainsMutationBarrierUntilCompletion(t *testing.T) {
	ctx := context.Background()
	s, e := New(filepath.Join(t.TempDir(), "catalog"), testOwner)
	if e != nil {
		t.Fatal(e)
	}
	if e = s.transaction(ctx, true, func(state *Snapshot) error { state.Guards["unresolved"] = 1; return nil }); e != nil {
		t.Fatal(e)
	}
	scope := memory.Scope{Type: memory.ScopeUser, Value: "_user"}
	if _, e = s.Put(ctx, scope, memory.Record{Kind: "fact", Key: "late", Content: "unsafe"}); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal("expired sender lost barrier", e)
	}
	if ok, e := s.Revalidate(ctx, "unresolved", "release", nil, nil, nil); e != nil || !ok {
		t.Fatal(ok, e)
	}
	if _, e = s.Put(ctx, scope, memory.Record{Kind: "fact", Key: "late", Content: "safe after completion"}); e != nil {
		t.Fatal(e)
	}
}
