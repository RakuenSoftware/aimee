package engine

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/internal/workflowstore"
)

func regressionCandidate(signature, state, suite, originRef, task string) workflowstore.EvalCandidate {
	return workflowstore.EvalCandidate{
		Signature: signature,
		State:     state,
		Suite:     suite,
		TaskName:  signature,
		OriginRef: originRef,
		TaskJSON:  task,
	}
}

func TestSelectAdmittedRegressionsUsesOnlyExactDeterministicEvidence(t *testing.T) {
	changed := []string{"src/zeta.c", "src/alpha.c"}
	candidates := []workflowstore.EvalCandidate{
		regressionCandidate(strings.Repeat("d", 32), "admitted", "suite", "", `{"prompt":"inspect src/alpha.cxx"}`),
		regressionCandidate(strings.Repeat("c", 32), "rejected", "suite", "src/alpha.c", `{"prompt":"ignored"}`),
		regressionCandidate(strings.Repeat("b", 32), "admitted", "suite", "src/zeta.c:widget", `{"prompt":"origin match"}`),
		regressionCandidate(strings.Repeat("a", 32), "admitted", "suite", "", `{"prompt":"prompt match for src/alpha.c, then verify it"}`),
		regressionCandidate(strings.Repeat("e", 32), "admitted", "suite", "", `{"prompt":"explicit","provenance":{"paths":["src/zeta.c"]}}`),
	}

	manifest, selected := selectAdmittedRegressions(changed, candidates)
	got := make([]string, 0, len(selected))
	for _, candidate := range selected {
		got = append(got, candidate.Signature)
	}
	want := []string{strings.Repeat("a", 32), strings.Repeat("b", 32), strings.Repeat("e", 32)}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("selected signatures = %v, want %v", got, want)
	}
	if manifest.Selected[0].Reason != "prompt_exact_path" ||
		manifest.Selected[1].Reason != "origin_ref" ||
		manifest.Selected[2].Reason != "provenance_path" {
		t.Fatalf("unexpected stable reasons: %#v", manifest.Selected)
	}
	if manifest.Excluded["state_not_admitted"] != 1 ||
		manifest.Excluded["no_exact_provenance_match"] != 1 {
		t.Fatalf("unexpected exclusions: %#v", manifest.Excluded)
	}

	again, _ := selectAdmittedRegressions([]string{"src/alpha.c", "src/zeta.c"}, candidates)
	manifest.RepositoryRevision, again.RepositoryRevision = "head", "head"
	manifest.DiffDigest, again.DiffDigest = "diff", "diff"
	if manifestDigest(manifest) != manifestDigest(again) {
		t.Fatal("identical inputs produced different manifest digests")
	}
}

func TestCommandVerifierRunsOnlyLedgerIdenticalAdmittedTask(t *testing.T) {
	root := t.TempDir()
	taskJSON := `{"prompt":"exercise src/alpha.c","assertions":[{"type":"contains","value":"ok"}]}`
	admittedPath := filepath.Join(root, "admitted.json")
	if err := os.WriteFile(admittedPath, []byte(taskJSON+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	candidate := regressionCandidate(strings.Repeat("a", 32), "admitted", "regressions", "", taskJSON)
	candidate.AdmittedPath = admittedPath
	verifier := CommandVerifier{
		LockFile: filepath.Join(root, "verify.lock"),
		RegressionCommand: []string{"sh", "-c",
			`count=$(find "$1" -type f -name '*.json' | wc -l); printf '{"status":"ok","passes":%s,"total":%s}\n' "$count" "$count"`, "runner"},
	}
	if err := verifier.VerifyAdmitted(context.Background(), root, []workflowstore.EvalCandidate{candidate}); err != nil {
		t.Fatalf("VerifyAdmitted: %v", err)
	}
	if err := os.WriteFile(admittedPath, []byte(`{"prompt":"replacement"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := verifier.VerifyAdmitted(context.Background(), root, []workflowstore.EvalCandidate{candidate}); err == nil || !strings.Contains(err.Error(), "no longer matches its ledger bytes") {
		t.Fatalf("tamper error = %v", err)
	}
}

func TestCommandVerifierRejectsNonAdmittedAndUnsafeCandidates(t *testing.T) {
	verifier := CommandVerifier{LockFile: filepath.Join(t.TempDir(), "verify.lock")}
	for _, candidate := range []workflowstore.EvalCandidate{
		regressionCandidate(strings.Repeat("a", 32), "rejected", "suite", "", `{}`),
		regressionCandidate(strings.Repeat("a", 32), "admitted", "../suite", "", `{}`),
		regressionCandidate("not-a-signature", "admitted", "suite", "", `{}`),
	} {
		if err := verifier.VerifyAdmitted(context.Background(), t.TempDir(), []workflowstore.EvalCandidate{candidate}); err == nil {
			t.Fatalf("unsafe candidate unexpectedly accepted: %#v", candidate)
		}
	}
}
