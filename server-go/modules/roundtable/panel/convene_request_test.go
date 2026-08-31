package panel

import (
	"context"
	"strings"
	"testing"
)

// A panel that cannot see the request cannot do its job.
//
// Every scope rule in the seat prompt is stated RELATIVE to the original
// request: "adding work the request did not ask for is drift", the
// requirement_coverage enumeration, "judge the artifact in front of you". With
// an empty ORIGINAL_REQUEST_DATA block all of them silently no-op and the seat
// falls back to grading the diff on its own merits -- generic code review,
// which rewards thoroughness and therefore APPROVES work the ticket never asked
// for.
//
// Observed on am_270b3483d5: the agent moved trust-bundle content validation
// into a preflight the codebase documents as presence-only, rewrote the comment
// that documents it, and the panel approved it twice. original_request was
// optional in the MCP tool schema, so the panel had nothing to compare against.
//
// Refusing beats approving. An approval a review could not have earned is worse
// than no review, because the caller records it as one.
func TestConveneRefusesAReviewWithNoOriginalRequest(t *testing.T) {
	for _, missing := range []string{"", "   ", "\n\t "} {
		run := Run{
			ID:              "run-1",
			OriginalRequest: missing,
			Reviewed:        Artifact{Content: "diff --git a/x b/x", Hash: "abc", Stage: "frozen_diff"},
		}
		_, err := Convene(context.Background(), nil, run, Panel{}, "")
		if err == nil {
			t.Fatalf("original_request %q was accepted; the panel must refuse", missing)
		}
		var ve ValidationError
		if !asValidationError(err, &ve) {
			t.Fatalf("expected ValidationError so the caller stops rather than retrying, got %T", err)
		}
		if !strings.Contains(err.Error(), "original_request") {
			t.Fatalf("error must name the missing field, got %q", err.Error())
		}
	}
}

func asValidationError(err error, out *ValidationError) bool {
	ve, ok := err.(ValidationError)
	if ok {
		*out = ve
	}
	return ok
}
