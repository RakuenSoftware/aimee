package executionpolicy

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Synthetic parser input only. This is not a measured task experiment and must
// never be installed as a deployment calibration artifact.
func calibrationFixture(t *testing.T, c explorationContract, now time.Time) explorationCalibration {
	t.Helper()
	report := map[string]any{
		"schema_version": 1, "manifest_sha256": strings.Repeat("a", 64), "results_sha256": strings.Repeat("b", 64),
		"expected_pairs": 500, "measured_pairs": 500, "invalid_cells": []any{}, "decision": "eligible_for_operator_review",
		"paired_interval_method": "bonferroni_clopper_pearson_discordant_rates_95_percent",
		"policy": map[string]float64{"task_success_noninferiority_margin": .01, "paired_confidence": .95,
			"maximum_p95_latency_ratio": 1.10, "maximum_total_cost_ratio": 1, "minimum_redundant_scan_reduction": .20},
		"gates": map[string]bool{"task_success_noninferior": true, "p95_latency": true, "total_cost": true, "redundant_scans": true},
		"metrics": map[string]any{"paired_success_interval": []float64{-.009, .009}, "arms": map[string]any{
			"baseline":  map[string]any{"task_successes": 500, "raw_scans": 1000, "redundant_scans": 500, "restrictions": 0, "false_restrictions": 0, "expansions": 0, "total_cost": 100, "p95_latency_seconds": 10},
			"treatment": map[string]any{"task_successes": 500, "raw_scans": 500, "redundant_scans": 0, "restrictions": 500, "false_restrictions": 0, "expansions": 5, "total_cost": 95, "p95_latency_seconds": 10},
		}},
	}
	raw, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	h := sha256.Sum256(raw)
	return explorationCalibration{Version: 1, ReviewedBy: "parser-test-only", Created: now.Add(-time.Minute), Expires: now.Add(time.Hour),
		Scope: calibrationScope(c), Limits: c.Limits, ReportSHA256: hex.EncodeToString(h[:]), Report: raw}
}

func calibrationContract(now time.Time) explorationContract {
	c := testContract(now)
	c.Binding.Project, c.Binding.Workspace, c.Binding.WorkingDirectory = "project", "local:project", "/project"
	c.Binding.WorktreeGeneration, c.Binding.IndexGeneration = "git-clean:"+strings.Repeat("a", 40), "9007199254740993"
	c.Binding.IndexObservedCurrent, c.Binding.OwnerObservedCurrent = true, true
	c.Binding.HostWorktree = true
	c.Binding.Route, c.Binding.Provider, c.Binding.Model = "native", "openai", "pinned-model"
	c.Binding.ProducerBuild = "0.4.5-test-only"
	c.Binding.LimitsDigest, c.ReceiptDigest = strings.Repeat("c", 64), strings.Repeat("d", 64)
	c.Binding.PlanDigest, c.Binding.SourceVersionsDigest = c.PlanDigest, c.SourceVersionsDigest
	c.QueryClass, c.CoverageComplete = "typed_requirements", true
	c.SupportedClasses = []string{"raw_scan"}
	c.Limits = explorationLimits{Enabled: true, RawScans: ceiling(0)}
	return c
}

func TestExplorationCalibrationRejectsChangedLiveScopeAndGates(t *testing.T) {
	now := time.Now()
	c := calibrationContract(now)
	a := calibrationFixture(t, c, now)
	encode := func(a explorationCalibration) []byte {
		b, e := json.Marshal(a)
		if e != nil {
			t.Fatal(e)
		}
		return b
	}
	raw := encode(a)
	if got, err := parseExplorationCalibration(raw, c, now); err != nil || got != a.ReportSHA256 {
		t.Fatal(got, err)
	}
	pretty, err := json.MarshalIndent(a, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if got, err := parseExplorationCalibration(pretty, c, now); err != nil || got != a.ReportSHA256 {
		t.Fatal("envelope formatting changed report commitment", got, err)
	}
	for _, change := range []func(*explorationContract){
		func(c *explorationContract) { c.Binding.HostWorktree = false },
		func(c *explorationContract) { c.Binding.ProducerBuild = "different-build" },
		func(c *explorationContract) { c.CoverageComplete = false }, func(c *explorationContract) { c.Binding.IndexObservedCurrent = false },
		func(c *explorationContract) { c.Binding.OwnerObservedCurrent = false }, func(c *explorationContract) { c.Binding.Model = "other" },
		func(c *explorationContract) { c.Binding.Project = "other" }, func(c *explorationContract) { c.Binding.IndexGeneration = "4" },
		func(c *explorationContract) { c.Binding.WorktreeGeneration = "unavailable" }, func(c *explorationContract) { c.ReceiptDigest = "" },
		func(c *explorationContract) { c.Limits.RawScans = ceiling(1) }, func(c *explorationContract) { c.Limits.Bytes = ceiling(1024) },
		func(c *explorationContract) { c.Binding.LimitsDigest = strings.Repeat("e", 64) },
	} {
		changed := c
		change(&changed)
		if _, err := parseExplorationCalibration(raw, changed, now); err == nil {
			t.Fatalf("changed contract accepted: %+v", changed)
		}
	}
	for _, change := range []func(*explorationCalibration){
		func(a *explorationCalibration) { a.Expires = now }, func(a *explorationCalibration) { a.Created = now.Add(time.Minute) },
		func(a *explorationCalibration) { a.Expires = now.Add(31 * 24 * time.Hour) }, func(a *explorationCalibration) { a.ReviewedBy = "" },
		func(a *explorationCalibration) { a.ReportSHA256 = strings.Repeat("0", 64) },
	} {
		changed := a
		change(&changed)
		if _, err := parseExplorationCalibration(encode(changed), c, now); err == nil {
			t.Fatal("invalid review accepted")
		}
	}
	for _, pair := range [][2]string{
		{`"eligible_for_operator_review"`, `"observe"`}, {`"measured_pairs":500`, `"measured_pairs":499`},
		{`"invalid_cells":[]`, `"invalid_cells":[{}]`}, {`"paired_success_interval":[-0.009,0.009]`, `"paired_success_interval":[-0.02,0.009]`},
		{`"maximum_p95_latency_ratio":1.1`, `"maximum_p95_latency_ratio":2`}, {`"redundant_scans":true`, `"redundant_scans":false`},
		{`"total_cost":95`, `"total_cost":101`}, {`"redundant_scans":0`, `"redundant_scans":401`},
	} {
		changed := strings.Replace(string(a.Report), pair[0], pair[1], 1)
		if changed == string(a.Report) {
			t.Fatal("fixture mutation did not apply", pair)
		}
		if reviewedExplorationReport([]byte(changed)) {
			t.Fatal("failed gate accepted", pair)
		}
	}
	t.Setenv("AIMEE_EXPLORATION_ENFORCE", "0")
	if approvedExplorationCalibration(c, now) != "" {
		t.Fatal("missing opt-in activated")
	}
}

func TestSessionCalibrationAdmissionRevocationAndOwnerRestart(t *testing.T) {
	testSessionApprovalAdmission(t, false)
}

func TestSessionExperimentAdmissionRevocationAndOwnerRestart(t *testing.T) {
	testSessionApprovalAdmission(t, true)
}

func testSessionApprovalAdmission(t *testing.T, experiment bool) {
	now := time.Now()
	c := calibrationContract(now)
	c.Binding.Task = "session-task"
	root := t.TempDir()
	for _, args := range [][]string{{"init", "-b", "main"}, {"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--allow-empty", "-m", "fixture"}} {
		if out, e := exec.Command("git", append([]string{"-C", root}, args...)...).CombinedOutput(); e != nil {
			t.Fatal(e, string(out))
		}
	}
	c.Binding.WorkingDirectory = root
	c.Binding.WorktreeGeneration = explorationWorktreeGeneration(root)
	home := t.TempDir()
	t.Setenv("AIMEE_HOME", home)
	if e := os.WriteFile(filepath.Join(home, "policy.json"), []byte(`{"exploration":{"raw_scans":2}}`), 0600); e != nil {
		t.Fatal(e)
	}
	a := calibrationFixture(t, c, now)
	raw, _ := json.Marshal(a)
	manifest := strings.Repeat("a", 64)
	if experiment {
		raw, _ = json.Marshal(explorationExperiment{Version: 1, Kind: "experiment", AuthorizedBy: "test-only",
			Created: now.Add(-time.Minute), Expires: now.Add(time.Hour), ManifestSHA256: manifest,
			Principal: c.Binding.Principal, Sessions: []string{c.Binding.Session}, Scope: calibrationScope(c), Limits: c.Limits})
	}
	optIn := true
	approve := func(c explorationContract, now time.Time) string {
		if !optIn {
			return ""
		}
		if experiment {
			receipt, _ := parseExplorationExperiment(raw, c, now, manifest)
			return receipt
		}
		receipt, _ := parseExplorationCalibration(raw, c, now)
		return receipt
	}
	var state []byte
	apply := func(req sessionExplorationRequest) map[string]any {
		t.Helper()
		wire, _ := json.Marshal(req)
		next, reply, e := sessionExploration("alice", "session", state, wire, now, approve)
		if e != nil {
			t.Fatal(e)
		}
		state = next
		var result map[string]any
		json.Unmarshal(reply, &result)
		return result
	}
	issued := apply(sessionExplorationRequest{Operation: "issue", Binding: c.Binding, Contract: &c})
	if issued["contract"].(map[string]any)["tier"] != "enforce" {
		t.Fatal(issued)
	}
	kind := "calibration"
	if experiment {
		kind = "experiment"
	}
	if issued["contract"].(map[string]any)["approval_kind"] != kind {
		t.Fatal("approval provenance lost", issued)
	}
	check := sessionExplorationRequest{Operation: "check", Binding: c.Binding, Tool: "grep", Arguments: json.RawMessage(`{"path":".","pattern":"symbol"}`), AttemptID: "restricted",
		IndexObservation: &explorationIndexObservation{Generation: c.Binding.IndexGeneration, HTTPStatus: 200, Body: `{"status":"ok","project":"project"}`},
		OwnerObservation: &explorationOwnerObservation{Status: "ok", MemoryOwner: c.Binding.MemoryOwner, PlanDigest: c.PlanDigest, SourceVersionsDigest: c.SourceVersionsDigest}}
	if result := apply(check); result["allowed"] != false || result["exploration"].(map[string]any)["mode"] != "enforce" {
		t.Fatal(result)
	}
	check.Operation, check.AttemptID = "check_session", "hook-restricted"
	check.Binding = explorationBinding{}
	check.OwnerObservation = &explorationOwnerObservation{Status: "ok", MemoryOwner: c.Binding.MemoryOwner, GenerationOnly: true}
	if result := apply(check); result["allowed"] != false || result["exploration"].(map[string]any)["mode"] != "enforce" {
		t.Fatal("hook admission differed from native", result)
	}
	check.Operation, check.Binding = "check", c.Binding
	// Revocation takes effect on the next distinct admission, preserving state.
	optIn = false
	check.AttemptID = "revoked"
	if result := apply(check); result["allowed"] != true {
		t.Fatal(result)
	}
	optIn = true
	check.AttemptID = "owner-restarted"
	check.OwnerObservation = nil
	if result := apply(check); result["allowed"] != true || result["exploration"].(map[string]any)["reason"] != "contract_invalidated" {
		t.Fatal(result)
	}
	check.AttemptID = "operator-still-exhausted"
	if result := apply(check); result["allowed"] != false || result["reason"] != "operator_exploration_budget_exhausted" {
		t.Fatal(result)
	}
}
