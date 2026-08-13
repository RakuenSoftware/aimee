package db1

import (
	"path/filepath"
	"sync"
	"testing"
)

// Separate Store values use separate SQLite connections. Starting both claims
// together exercises database-level writer contention rather than relying on a
// Store's one-connection serialization. ClaimFrozenCreates reserves the writer
// before its conflict read, so the second connection must observe the first
// connection's committed claim and return a structured collision.
func TestClaimFrozenCreatesSerializesTwoConnections(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	first, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()

	ctx := t.Context()
	for _, item := range []CreateWorkItem{
		{ID: "wi_parent", Repo: "repo", ProposalPath: "parent", WorkflowName: "build", StartStage: "slices"},
		{ID: "wi_slice_a", Repo: "repo", ProposalPath: "slice-a", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
		{ID: "wi_slice_b", Repo: "repo", ProposalPath: "slice-b", WorkflowName: "slice", StartStage: "freeze", ParentID: "wi_parent"},
	} {
		if err := first.CreateWorkItem(ctx, item); err != nil {
			t.Fatal(err)
		}
	}

	stores := []*Store{first, second}
	ids := []string{"wi_slice_a", "wi_slice_b"}
	conflicts := make([]*FrozenCreateConflict, len(stores))
	errs := make([]error, len(stores))
	start := make(chan struct{})
	var wg sync.WaitGroup
	for i := range stores {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			conflicts[i], errs[i] = stores[i].ClaimFrozenCreates(ctx, "wi_parent", ids[i], []FrozenCreate{{
				Path: "docs/appliance-runbook.md", ContentHash: "blob-" + ids[i],
			}})
		}(i)
	}
	close(start)
	wg.Wait()

	winners, losers := 0, 0
	for i := range stores {
		if errs[i] != nil {
			t.Fatalf("claim %s returned generic contention error: %v", ids[i], errs[i])
		}
		if conflicts[i] == nil {
			winners++
			continue
		}
		losers++
		if conflicts[i].Path != "docs/appliance-runbook.md" ||
			conflicts[i].ConflictingWorkItem != ids[i] ||
			(conflicts[i].ExistingWorkItem != ids[0] && conflicts[i].ExistingWorkItem != ids[1]) {
			t.Fatalf("claim %s conflict=%+v", ids[i], conflicts[i])
		}
	}
	if winners != 1 || losers != 1 {
		t.Fatalf("winners=%d losers=%d conflicts=%+v", winners, losers, conflicts)
	}
}
