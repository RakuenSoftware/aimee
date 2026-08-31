package db1_test

// Tests about the generated wire surface rather than about the store.
//
// The Go client is generated from the same catalog as the C one, and most of
// its shapes are covered incidentally: the engine calls them, so the engine's
// tests exercise them. One shape is not. An operation whose reply is SEVERAL
// loose scalars -- no struct, no list, just three numbers -- has no caller in
// the engine, so the emitter's handling of it was never run.
//
// It was also wrong. The emitter returned only the first cell, which for
// work_item_child_counts meant a caller asking "how many children, how many
// accepted, how many failed" received the total and believed it had all three.
// That is the kind of defect that produces a confidently wrong answer rather
// than an error, so it is worth a test even though nothing calls it today --
// and especially because nothing calls it today, since the next caller would
// have inherited the bug.

import (
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/db1/db1test"
)

func TestSeveralLooseScalarsAllComeBack(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aimee.db")
	store, err := db1test.Open(t, path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	client := db1test.Client(t, path)
	ctx := t.Context()

	parent := db1.CreateWorkItem{ID: "wi_counts_parent", Repo: "repo", ProposalPath: "counts",
		WorkflowName: "build", StartStage: "slices"}
	if err := store.CreateWorkItem(ctx, parent); err != nil {
		t.Fatal(err)
	}
	// Three children with three different fates, so a reply that carried only
	// one number could not accidentally look right.
	children := []struct {
		id    string
		state string
	}{
		{"wi_counts_ok", "accepted"},
		{"wi_counts_bad", "rejected"},
		{"wi_counts_live", ""},
	}
	for _, child := range children {
		in := db1.CreateWorkItem{ID: child.id, Repo: "repo", ProposalPath: child.id,
			WorkflowName: "slice", StartStage: "work", ParentID: parent.ID}
		if err := store.CreateWorkItem(ctx, in); err != nil {
			t.Fatal(err)
		}
		if child.state == "" {
			continue
		}
		if err := client.WorkItemSetTerminal(ctx, child.id, child.state); err != nil {
			t.Fatalf("set %s terminal: %v", child.id, err)
		}
	}

	total, accepted, failed, err := client.WorkItemChildCounts(ctx, parent.ID)
	if err != nil {
		t.Fatalf("child counts: %v", err)
	}
	if total != 3 {
		t.Errorf("total = %d, want 3", total)
	}
	if accepted != 1 {
		t.Errorf("accepted = %d, want 1", accepted)
	}
	// The one that matters: before the emitter was fixed this was the total,
	// because only the first cell of the reply was read.
	if failed != 1 {
		t.Errorf("failed = %d, want 1 -- a reply of several scalars must not be "+
			"truncated to its first cell", failed)
	}
	if total == accepted && accepted == failed {
		t.Error("all three counts are equal, which is what reading one cell three times looks like")
	}
}
