package engine

import (
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/db1/db1test"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func TestValidateStructuredRejectsInvalidPacketDependencies(t *testing.T) {
	tests := []struct {
		name string
		doc  string
		want string
	}{
		{
			name: "unknown",
			doc:  `{"schema_version":1,"packets":[{"packet_id":"p1","dependencies":["missing"],"acceptance_criteria":["done"]}]}`,
			want: "unknown packet missing",
		},
		{
			name: "self",
			doc:  `{"schema_version":1,"packets":[{"packet_id":"p1","dependencies":["p1"],"acceptance_criteria":["done"]}]}`,
			want: "cannot depend on itself",
		},
		{
			name: "cycle",
			doc:  `{"schema_version":1,"packets":[{"packet_id":"p1","dependencies":["p2"],"acceptance_criteria":["one"]},{"packet_id":"p2","dependencies":["p1"],"acceptance_criteria":["two"]}]}`,
			want: "dependency cycle",
		},
		{
			name: "non-array",
			doc:  `{"schema_version":1,"packets":[{"packet_id":"p1","dependencies":"p0","acceptance_criteria":["done"]}]}`,
			want: "dependencies must be an array",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			err := validateStructured("packets", []byte(test.doc))
			if err == nil || !strings.Contains(err.Error(), test.want) {
				t.Fatalf("error=%v, want substring %q", err, test.want)
			}
		})
	}
}

func TestPacketDependencyGateWaitsForAcceptedPredecessors(t *testing.T) {
	store, err := db1test.Open(t, filepath.Join(t.TempDir(), "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(t.TempDir(), "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	const parentID = "wi_parent"
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
		ID: parentID, Repo: "repo", ProposalPath: "parent", WorkflowName: "build", StartStage: "slices",
	}); err != nil {
		t.Fatal(err)
	}
	// A prior refinement generation may carry the same packet IDs. Its terminal
	// state must not satisfy or poison dependencies in the current generation.
	const priorID = parentID + ".sold.g0.0"
	if err := artifacts.PutProposal(priorID, []byte(`{"packet_id":"p1","dependencies":[]}`)); err != nil {
		t.Fatal(err)
	}
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
		ID: priorID, Repo: "repo", ProposalPath: priorID, WorkflowName: "slice",
		StartStage: "scope", ParentID: parentID,
	}); err != nil {
		t.Fatal(err)
	}
	if err := store.Finish(t.Context(), priorID, "scope", "rejected", "old generation", "", 0); err != nil {
		t.Fatal(err)
	}
	items := []struct {
		id       string
		proposal string
	}{
		{id: parentID + ".sabc.g0.0", proposal: `{"packet_id":"p1","dependencies":[]}`},
		{id: parentID + ".sabc.g0.1", proposal: `{"packet_id":"p2","dependencies":["p1"]}`},
	}
	for _, input := range items {
		if err := artifacts.PutProposal(input.id, []byte(input.proposal)); err != nil {
			t.Fatal(err)
		}
		if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
			ID: input.id, Repo: "repo", ProposalPath: input.id, WorkflowName: "slice",
			StartStage: "scope", ParentID: parentID,
		}); err != nil {
			t.Fatal(err)
		}
	}
	agents := &recordingAgents{}
	runner := &NativeRunner{db: store, artifacts: artifacts, agents: agents}
	dependent, err := store.WorkItem(t.Context(), items[1].id)
	if err != nil {
		t.Fatal(err)
	}
	result, blocked, err := runner.packetDependencyGate(t.Context(), dependent)
	if err != nil || !blocked || result.Status != StepPending || result.PauseReason != "dependency_pending" {
		t.Fatalf("active dependency result=%+v blocked=%v err=%v", result, blocked, err)
	}
	result, err = runner.Run(t.Context(), StepRequest{
		WorkItem: dependent,
		Node:     wfe.Node{ID: "scope", Block: "understand"},
		Proposal: items[1].proposal,
	})
	if err != nil || result.Status != StepPending || result.PauseReason != "dependency_pending" {
		t.Fatalf("runner dependency gate result=%+v err=%v", result, err)
	}
	if len(agents.requests) != 0 {
		t.Fatalf("dependency-blocked slice dispatched %d delegate request(s)", len(agents.requests))
	}
	if err := store.Finish(t.Context(), items[0].id, "scope", "accepted", "merged", "", 0); err != nil {
		t.Fatal(err)
	}
	result, blocked, err = runner.packetDependencyGate(t.Context(), dependent)
	if err != nil || blocked {
		t.Fatalf("accepted dependency result=%+v blocked=%v err=%v", result, blocked, err)
	}
}

func TestPacketDependencyGateFailsClosedOnRejectedPredecessor(t *testing.T) {
	store, err := db1test.Open(t, filepath.Join(t.TempDir(), "aimee.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(t.TempDir(), "artifacts"))
	if err != nil {
		t.Fatal(err)
	}
	const parentID = "wi_parent"
	if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
		ID: parentID, Repo: "repo", ProposalPath: "parent", WorkflowName: "build", StartStage: "slices",
	}); err != nil {
		t.Fatal(err)
	}
	for i, proposal := range []string{
		`{"packet_id":"p1","dependencies":[]}`,
		`{"packet_id":"p2","dependencies":["p1"]}`,
	} {
		id := parentID + ".sabc.g0." + string(rune('0'+i))
		if err := artifacts.PutProposal(id, []byte(proposal)); err != nil {
			t.Fatal(err)
		}
		if err := store.CreateWorkItem(t.Context(), db1.CreateWorkItem{
			ID: id, Repo: "repo", ProposalPath: id, WorkflowName: "slice",
			StartStage: "scope", ParentID: parentID,
		}); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Finish(t.Context(), parentID+".sabc.g0.0", "scope", "rejected", "failed", "", 0); err != nil {
		t.Fatal(err)
	}
	dependent, err := store.WorkItem(t.Context(), parentID+".sabc.g0.1")
	if err != nil {
		t.Fatal(err)
	}
	result, blocked, err := (&NativeRunner{db: store, artifacts: artifacts}).packetDependencyGate(t.Context(), dependent)
	if err != nil || !blocked || result.Status != StepFailed || !strings.Contains(result.Detail, "ended rejected") {
		t.Fatalf("result=%+v blocked=%v err=%v", result, blocked, err)
	}
}
