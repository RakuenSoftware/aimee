package learning

// Configured tests establish a result on one source snapshot. They do not
// establish semantic task correctness or that an exposed procedure was applied.
import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"strings"
	"time"

	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

// Wire ownership lives in canonical IR; qualification remains this module’s policy.
type ExecutionTestStep = canonical.ExecutionTestStep
type ExecutionTestProof = canonical.ExecutionTestProof

type ExecutionTestGrade struct {
	SchemaVersion  int                `json:"schema_version"`
	Source         ExecutionSource    `json:"source"`
	ObservedStatus string             `json:"observed_status"`
	Verification   string             `json:"verification"`
	Qualification  string             `json:"qualification"`
	Verdict        string             `json:"verdict"`
	VerificationID string             `json:"verification_ref"`
	Proof          ExecutionTestProof `json:"proof"`
}

type ExecutionTestGradeRequest struct {
	Record ExecutionRecord    `json:"record"`
	Proof  ExecutionTestProof `json:"proof"`
}

func handleExecutionTestGrade(args []byte, caller ring.CommandContext) ([]byte, ring.ModuleStatus) {
	var request ExecutionTestGradeRequest
	if !canonical.Decode(args, &request) {
		return nil, ring.ModuleStatusInvalidRequest
	}
	grade, ok := executionTestGrade(request.Record, request.Proof, caller)
	if !ok {
		return nil, ring.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(map[string]any{"status": "ok", "grade": grade})
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	framed, err := ring.EncodeCommandResult(body)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	return framed, ring.ModuleStatusOK
}

func lowercaseHex(value string, lengths ...int) bool {
	allowed := false
	for _, length := range lengths {
		allowed = allowed || len(value) == length
	}
	if !allowed {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func executionTestGrade(record ExecutionRecord, proof ExecutionTestProof, caller ring.CommandContext) (ExecutionTestGrade, bool) {
	var empty ExecutionTestGrade
	if caller.TransportIdentity != "host:isolated-project-verification" {
		return empty, false
	}
	terminalCaller := caller
	terminalCaller.TransportIdentity = "host:canonical-execution"
	observation, ok := executionObservation(record, terminalCaller)
	if !ok || observation.ObservedStatus != "completed" || proof.Version != 1 ||
		proof.Session != record.Session || proof.Turn != record.Turn || proof.Actor != record.Actor ||
		proof.ScopeKind != "project" || proof.ScopeID == "" || len(proof.ScopeID) > 256 ||
		strings.ContainsRune(proof.ScopeID, 0) || proof.Isolation != "private_source_snapshot" ||
		!proof.Completed || !lowercaseHex(proof.SnapshotID, 32) ||
		!lowercaseHex(proof.SnapshotCommit, 40, 64) || proof.SnapshotCommit != proof.FinalCommit ||
		!lowercaseHex(proof.SnapshotTree, 40, 64) || proof.SnapshotTree != proof.FinalTree ||
		!lowercaseHex(proof.ConfigurationDigest, 64) || proof.ConfigurationDigest != proof.FinalConfigurationDigest ||
		!lowercaseHex(proof.ProfileDigest, 64) || len(proof.Steps) == 0 || len(proof.Steps) > 64 {
		return empty, false
	}
	if at, err := time.Parse(time.RFC3339Nano, proof.ObservedAt); err != nil || at.IsZero() {
		return empty, false
	}
	verdict := "passed"
	seen := map[string]bool{}
	for _, step := range proof.Steps {
		if step.Name == "" || len(step.Name) > 128 || strings.ContainsRune(step.Name, 0) || seen[step.Name] ||
			!lowercaseHex(step.CommandDigest, 64) || !step.Ran || step.Skipped == nil || *step.Skipped ||
			step.Termination != "exited" {
			return empty, false
		}
		seen[step.Name] = true
		code, err := strconv.Atoi(step.ExitCode)
		if err != nil || code < 0 || code > 255 || strconv.Itoa(code) != step.ExitCode {
			return empty, false
		}
		if code != 0 {
			verdict = "failed"
		}
	}
	encoded, err := json.Marshal(proof)
	if err != nil {
		return empty, false
	}
	digest := sha256.Sum256(encoded)
	return ExecutionTestGrade{
		SchemaVersion: 1, Source: observation.Source, ObservedStatus: observation.ObservedStatus,
		Verification: "tests_bound_revision", Qualification: "configured_tests_only", Verdict: verdict,
		VerificationID: "sha256:" + hex.EncodeToString(digest[:]), Proof: proof,
	}, true
}
