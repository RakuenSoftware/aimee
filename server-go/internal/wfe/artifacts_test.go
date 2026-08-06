package wfe

import (
	"errors"
	"io/fs"
	"os"
	"strings"
	"testing"
)

func TestArtifactsAreLosslessAndProposalIsImmutable(t *testing.T) {
	store, err := NewArtifactStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}

	const workItemID = "wi_lossless"
	proposalTail := "PROPOSAL_ACCEPTANCE_CRITERION_AT_THE_END"
	proposal := []byte("# Request\n\n" + strings.Repeat("proposal detail λ\n", 90_000) + proposalTail)
	planTail := "PLAN_PROOF_AT_THE_END"
	plan := []byte("# Plan\n\n" + strings.Repeat("implementation step 漢字\n", 90_000) + planTail)

	if err := store.PutProposal(workItemID, proposal); err != nil {
		t.Fatal(err)
	}
	if err := store.PutPlan(workItemID, plan); err != nil {
		t.Fatal(err)
	}

	packet, err := store.PlanReviewPacket(workItemID)
	if err != nil {
		t.Fatal(err)
	}
	if packet.Proposal != string(proposal) || !strings.HasSuffix(packet.Proposal, proposalTail) {
		t.Fatal("review packet did not preserve the complete proposal")
	}
	if packet.Plan != string(plan) || !strings.HasSuffix(packet.Plan, planTail) {
		t.Fatal("review packet did not preserve the complete plan")
	}
	if packet.PlanHash != Hash(plan) {
		t.Fatal("plan hash does not cover the complete plan")
	}

	if err := store.PutProposal(workItemID, proposal); err != nil {
		t.Fatalf("idempotent proposal write failed: %v", err)
	}
	if err := store.PutProposal(workItemID, []byte("replacement")); !errors.Is(err, ErrImmutable) {
		t.Fatalf("proposal replacement error = %v, want ErrImmutable", err)
	}
	gotProposal, err := store.Proposal(workItemID)
	if err != nil {
		t.Fatal(err)
	}
	if string(gotProposal) != string(proposal) {
		t.Fatal("writing the plan or replacing the proposal changed the immutable request")
	}
}

func TestFeedbackRoundTripsWithoutFieldLimits(t *testing.T) {
	store, err := NewArtifactStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}

	longSummary := strings.Repeat("full observed behavior; ", 80_000) + "SUMMARY_END"
	longRecommendation := strings.Repeat("complete corrective action; ", 80_000) + "RECOMMENDATION_END"
	want := ReviewFeedback{
		SchemaVersion: 1,
		ArtifactHash:  strings.Repeat("a", 64),
		Findings: []Finding{{
			ID:             "criterion-final",
			Persona:        "qa",
			Severity:       "blocking",
			Location:       "plan section 99",
			Summary:        longSummary,
			Recommendation: longRecommendation,
		}},
	}
	if err := store.PutFeedback("wi_feedback", want); err != nil {
		t.Fatal(err)
	}
	got, err := store.Feedback("wi_feedback")
	if err != nil {
		t.Fatal(err)
	}
	if len(got.Findings) != 1 || got.Findings[0].Summary != longSummary ||
		got.Findings[0].Recommendation != longRecommendation {
		t.Fatal("structured feedback did not round-trip losslessly")
	}
}

func TestArtifactPathsRejectTraversal(t *testing.T) {
	store, err := NewArtifactStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if err := store.PutProposal("../escape", []byte("x")); !errors.Is(err, ErrInvalidWorkItem) {
		t.Fatalf("path traversal error = %v, want ErrInvalidWorkItem", err)
	}
}

// TestNodeArtifactMissingSignalsFileNotExist locks down the missing-artifact
// contract that freeze_collision.go and other callers rely on: a NodeArtifact
// read of an absent node artifact must surface both fs.ErrNotExist
// (equivalently os.ErrNotExist) and the package's exported ErrArtifactNotExist
// sentinel through errors.Is. Any future read helper that loses this chain
// would cause the sibling-collision branch to misclassify a simply-absent
// freeze artifact as a generic I/O error and reject the current slice.
func TestNodeArtifactMissingSignalsFileNotExist(t *testing.T) {
	store, err := NewArtifactStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}

	_, err = store.NodeArtifact("wi_absent", "freeze")
	if err == nil {
		t.Fatal("expected an error from NodeArtifact on a missing work-item")
	}
	if !errors.Is(err, fs.ErrNotExist) {
		t.Fatalf("missing NodeArtifact error does not match fs.ErrNotExist: %v", err)
	}
	if !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("missing NodeArtifact error does not match os.ErrNotExist: %v", err)
	}
	if !errors.Is(err, ErrArtifactNotExist) {
		t.Fatalf("missing NodeArtifact error does not match ErrArtifactNotExist: %v", err)
	}
}
