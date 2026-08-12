package delegates

import "strings"

// Whether a review actually looked at the code it reviewed.
//
// A reviewing delegate can produce a confident, well-formatted report without
// having read anything -- citing line numbers that moved, or announcing that
// there is nothing to review while the worktree is full of changes. Both are
// indistinguishable from a real review by shape alone, so they are caught by
// comparing the report against the checkout it claims to describe.
//
// Two checks, and they are not the same question:
//
//   - The SNIPPET check asks whether quoted code still matches the file near
//     the line it cites. That needs the checkout, so the caller performs it;
//     what this decides is WHICH roles warrant it.
//   - The PARENT-DIFF check asks whether the report claims there was nothing to
//     look at while the worktree says otherwise. The caller supplies the one
//     fact it cannot compute here -- is the worktree dirty -- and the reading of
//     the claim happens here.

// guardedRoles are the roles whose output is checked at all. A role that is
// expected to change code is not here: its evidence is the diff it produced.
var guardedRoles = map[string]bool{
	"review": true, "validate": true, "diagnose": true,
	"test": true, "check": true, "inspect": true,
}

// snippetCheckedRoles are the roles whose citations are checked against the
// checkout. Narrower than guardedRoles on purpose: "diagnose" and "inspect"
// legitimately quote history, logs, and code that is no longer present, so
// holding them to the current checkout would fail correct work.
var snippetCheckedRoles = map[string]bool{
	"review": true, "validate": true, "test": true, "check": true,
}

// missingParentDiffClaims are the ways a report says "there was nothing to
// review". Any of them, in a dirty worktree, is a contradiction.
var missingParentDiffClaims = []string{
	"no uncommitted diff exists",
	"working tree is clean",
	"there is nothing to review",
	"cannot see the current diff",
	"cannot verify the diff",
}

// ReviewEvidenceFacts is what the caller knows and this side cannot look up.
type ReviewEvidenceFacts struct {
	Role     string
	Response string
	// TargetProvided means the review target travelled IN the prompt -- a diff
	// the caller pasted. Then the provided content IS the evidence, and there is
	// no host checkout to drift against; guarding against the local worktree
	// would penalise a correct review of exactly what it was handed.
	TargetProvided bool
	// WorktreeDirty is `git status --porcelain` finding anything, from the
	// caller, because it is I/O.
	WorktreeDirty bool
}

// ReviewEvidenceVerdict is what the caller should do next.
type ReviewEvidenceVerdict struct {
	// Guarded is false when this role is not checked at all; the caller stops.
	Guarded bool
	// CheckSnippets asks the caller to run the citation-vs-checkout comparison,
	// which is I/O. It fails the delegate with its own wording when it finds
	// drift; that check runs BEFORE the contradiction below, so the more
	// specific complaint wins.
	CheckSnippets bool
	// Contradiction is set when the report claims there was nothing to review
	// while the worktree says otherwise.
	Contradiction bool
	// Error is the operator-facing wording for Contradiction.
	Error string
}

// JudgeReviewEvidence reads a delegate's report for signs it did not look.
func JudgeReviewEvidence(f ReviewEvidenceFacts) ReviewEvidenceVerdict {
	if f.Response == "" || !guardedRoles[f.Role] {
		return ReviewEvidenceVerdict{}
	}
	if f.TargetProvided {
		return ReviewEvidenceVerdict{}
	}

	v := ReviewEvidenceVerdict{
		Guarded:       true,
		CheckSnippets: snippetCheckedRoles[f.Role],
	}

	if f.WorktreeDirty && claimsMissingParentDiff(f.Response) {
		v.Contradiction = true
		v.Error = "delegate evidence drift: response claimed the parent diff was clean or " +
			"missing while the parent worktree has uncommitted changes"
	}
	return v
}

func claimsMissingParentDiff(response string) bool {
	lower := strings.ToLower(response)
	for _, claim := range missingParentDiffClaims {
		if strings.Contains(lower, claim) {
			return true
		}
	}
	return false
}
