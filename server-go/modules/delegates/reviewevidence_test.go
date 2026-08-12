package delegates

import (
	"strings"
	"testing"
)

func TestUnguardedRolesAreLeftAlone(t *testing.T) {
	for _, role := range []string{"code", "execute", "explain", "plan", ""} {
		v := JudgeReviewEvidence(ReviewEvidenceFacts{
			Role: role, Response: "the working tree is clean", WorktreeDirty: true,
		})
		if v.Guarded || v.CheckSnippets || v.Contradiction {
			t.Errorf("role %q should not be guarded: %+v", role, v)
		}
	}
}

// "diagnose" and "inspect" are guarded but NOT snippet-checked: they quote
// history, logs, and code that has since moved, and holding those citations to
// the current checkout would fail correct work.
func TestOnlySomeGuardedRolesGetTheSnippetCheck(t *testing.T) {
	for _, role := range []string{"review", "validate", "test", "check"} {
		v := JudgeReviewEvidence(ReviewEvidenceFacts{Role: role, Response: "a report"})
		if !v.Guarded || !v.CheckSnippets {
			t.Errorf("role %q should be snippet-checked: %+v", role, v)
		}
	}
	for _, role := range []string{"diagnose", "inspect"} {
		v := JudgeReviewEvidence(ReviewEvidenceFacts{Role: role, Response: "a report"})
		if !v.Guarded {
			t.Errorf("role %q should still be guarded: %+v", role, v)
		}
		if v.CheckSnippets {
			t.Errorf("role %q must not be snippet-checked: it quotes what is no longer there", role)
		}
	}
}

// A caller-supplied target IS the evidence, so there is no checkout to drift
// against. Guarding here would penalise a correct review of the pasted diff.
func TestACallerSuppliedTargetDisablesTheGuard(t *testing.T) {
	v := JudgeReviewEvidence(ReviewEvidenceFacts{
		Role:           "review",
		Response:       "there is nothing to review",
		TargetProvided: true,
		WorktreeDirty:  true,
	})
	if v.Guarded || v.CheckSnippets || v.Contradiction {
		t.Errorf("a provided target should disable the guard entirely: %+v", v)
	}
}

func TestAnEmptyResponseIsNotJudged(t *testing.T) {
	v := JudgeReviewEvidence(ReviewEvidenceFacts{Role: "review", WorktreeDirty: true})
	if v.Guarded {
		t.Errorf("nothing to read, nothing to judge: %+v", v)
	}
}

// Ported from the C fixture test_review_evidence_guard_rejects_clean_claim_on_
// dirty_worktree: the SAME response is fine against a clean worktree and a
// contradiction against a dirty one. The claim is not wrong by itself -- it is
// wrong only next to the fact.
func TestPortedCleanClaimAgainstACleanThenDirtyWorktree(t *testing.T) {
	const response = "No uncommitted diff exists. The working tree is clean."

	clean := JudgeReviewEvidence(ReviewEvidenceFacts{Role: "review", Response: response})
	if clean.Contradiction {
		t.Errorf("a clean claim about a clean worktree is true: %+v", clean)
	}

	dirty := JudgeReviewEvidence(ReviewEvidenceFacts{
		Role: "validate", Response: response, WorktreeDirty: true,
	})
	if !dirty.Contradiction {
		t.Fatalf("a clean claim about a dirty worktree is drift: %+v", dirty)
	}
	if !strings.Contains(dirty.Error, "delegate evidence drift") {
		t.Errorf("error should say what it is: %q", dirty.Error)
	}
}

// The claim is matched case-insensitively, and the C fixture's own wording
// ("No uncommitted diff exists.") only matches because of that.
func TestClaimMatchingIgnoresCase(t *testing.T) {
	for _, response := range []string{
		"NO UNCOMMITTED DIFF EXISTS",
		"The Working Tree Is Clean",
		"There Is Nothing To Review",
		"I cannot see the current diff",
		"I Cannot Verify The Diff",
	} {
		v := JudgeReviewEvidence(ReviewEvidenceFacts{
			Role: "review", Response: response, WorktreeDirty: true,
		})
		if !v.Contradiction {
			t.Errorf("should have been read as a missing-diff claim: %q", response)
		}
	}
}

// A dirty worktree alone is not drift: a report that says nothing about whether
// there was a diff has not contradicted anything.
func TestADirtyWorktreeAloneIsNotDrift(t *testing.T) {
	v := JudgeReviewEvidence(ReviewEvidenceFacts{
		Role:          "review",
		Response:      "Finding: the parser drops a NUL. Location: src/a.c:12",
		WorktreeDirty: true,
	})
	if v.Contradiction {
		t.Errorf("no claim was made, so nothing contradicts: %+v", v)
	}
	if !v.CheckSnippets {
		t.Errorf("the citation still deserves the snippet check: %+v", v)
	}
}
