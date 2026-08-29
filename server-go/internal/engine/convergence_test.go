package engine

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func TestBlockerFingerprintSetIgnoresOrderIDsAndPresentation(t *testing.T) {
	a := &wfe.ReviewFeedback{Findings: []wfe.Finding{
		{ID: "invented-a", Persona: " Security ", Severity: "BLOCKING", Location: "src/a.c:12", Summary: "Validate the target_digest."},
		{ID: "invented-b", Persona: "architect", Severity: "foundational", Location: "src/b.c:7", Summary: "Keep ONE owner"},
	}}
	b := &wfe.ReviewFeedback{Findings: []wfe.Finding{
		{ID: "new-id", Persona: "ARCHITECT", Severity: "FOUNDATIONAL", Location: "src/b.c:7", Summary: "keep one owner!!!"},
		{ID: "another-id", Persona: "security", Severity: "blocking", Location: "src/a.c:12", Summary: "validate-the-target digest"},
	}}
	if got, want := blockerFingerprintSet("review", a), blockerFingerprintSet("review", b); got != want {
		t.Fatalf("presentation changed blocker identity:\n%s\n%s", got, want)
	}
}

func TestBlockerFingerprintSetChangesForObligationLocationOrGate(t *testing.T) {
	base := &wfe.ReviewFeedback{Findings: []wfe.Finding{{
		Persona: "security", Severity: "blocking", Location: "src/a.c:12", Summary: "validate target",
	}}}
	changed := &wfe.ReviewFeedback{Findings: []wfe.Finding{{
		Persona: "security", Severity: "blocking", Location: "src/a.c:13", Summary: "validate target",
	}}}
	if blockerFingerprintSet("review", base) == blockerFingerprintSet("review", changed) {
		t.Fatal("location change did not change blocker identity")
	}
	if blockerFingerprintSet("review", base) == blockerFingerprintSet("plan-review", base) {
		t.Fatal("gate change did not change blocker identity")
	}
}

func TestConvergencePayloadCarriesHumanSummaryAndBoundedSet(t *testing.T) {
	feedback := &wfe.ReviewFeedback{Findings: []wfe.Finding{{
		Persona: "security", Severity: "blocking", Summary: "bind authorization",
	}}}
	raw := convergencePayload("review", "still unresolved", feedback, "observe")
	var payload convergencePayloadV1
	if err := json.Unmarshal([]byte(raw), &payload); err != nil {
		t.Fatal(err)
	}
	if payload.Version != 1 || payload.Mode != "observe" || payload.Summary != "still unresolved" || len(payload.BlockerSet) != 64 {
		t.Fatalf("payload=%+v", payload)
	}

	feedback.Findings = make([]wfe.Finding, maxConvergenceBlockers+1)
	for i := range feedback.Findings {
		feedback.Findings[i] = wfe.Finding{Persona: "qa", Severity: "blocking", Summary: strings.Repeat("x", i+1)}
	}
	if got := blockerFingerprintSet("review", feedback); got != "" {
		t.Fatalf("oversized set should use conservative fallback, got %d bytes", len(got))
	}
}
