package ir

import (
	"encoding/json"
	"strconv"
	"strings"
)

type ExecutionTestStep struct {
	Name          string `json:"name"`
	CommandDigest string `json:"command_sha256"`
	Ran           bool   `json:"ran"`
	Skipped       *bool  `json:"skipped"`
	Termination   string `json:"termination"`
	ExitCode      string `json:"exit_code"`
}

// A host verifier supplies this only after its isolated worker has joined.
// Neither shared-worktree pre/post hashes nor native SDK claims establish
// private snapshot custody. The host producer must own that custody itself.
type ExecutionTestProof struct {
	Version                  int                 `json:"version"`
	Session                  string              `json:"session_id"`
	Turn                     string              `json:"turn_id"`
	Actor                    string              `json:"actor"`
	ScopeKind                string              `json:"scope_kind"`
	ScopeID                  string              `json:"scope_id"`
	Isolation                string              `json:"isolation"`
	SnapshotID               string              `json:"snapshot_id"`
	SnapshotCommit           string              `json:"snapshot_commit"`
	FinalCommit              string              `json:"final_commit"`
	SnapshotTree             string              `json:"snapshot_tree"`
	FinalTree                string              `json:"final_tree"`
	ConfigurationDigest      string              `json:"configuration_sha256"`
	FinalConfigurationDigest string              `json:"final_configuration_sha256"`
	ProfileDigest            string              `json:"profile_sha256"`
	Completed                bool                `json:"completed"`
	ObservedAt               string              `json:"observed_at"`
	Steps                    []ExecutionTestStep `json:"steps"`
}

// ExecutionPayload is shared by strict readers after canonical event admission.
type ExecutionPayload struct {
	Status             string               `json:"status"`
	Error              string               `json:"error,omitempty"`
	VerificationProofs []ExecutionTestProof `json:"verification_proofs,omitempty"`
}

// ValidVerificationProof checks a bounded metadata schema, not execution custody,
// correctness or eligibility for a grade. Learning owns those qualifications.
func ValidVerificationProof(encoded []byte) bool {
	if len(encoded) > 64<<10 {
		return false
	}
	var proof ExecutionTestProof
	var fields map[string]json.RawMessage
	if !Decode(encoded, &proof) || !Decode(encoded, &fields) || len(fields) != 18 || proof.Version != 1 {
		return false
	}
	text := func(value string, limit int) bool {
		return value != "" && len(value) <= limit && !strings.ContainsRune(value, 0)
	}
	hex := func(value string, lengths ...int) bool {
		for _, length := range lengths {
			if len(value) == length && strings.Trim(value, "0123456789abcdef") == "" {
				return true
			}
		}
		return false
	}
	var completed bool
	if !Decode(fields["completed"], &completed) ||
		!text(proof.Session, 256) || !text(proof.Turn, 256) || !text(proof.Actor, 256) ||
		proof.ScopeKind != "project" || !text(proof.ScopeID, 256) || proof.Isolation != "private_source_snapshot" ||
		!hex(proof.SnapshotID, 32) || !hex(proof.SnapshotCommit, 40, 64) || !hex(proof.FinalCommit, 40, 64) ||
		!hex(proof.SnapshotTree, 40, 64) || !hex(proof.FinalTree, 40, 64) ||
		!hex(proof.ConfigurationDigest, 64) || !hex(proof.FinalConfigurationDigest, 64) ||
		!hex(proof.ProfileDigest, 64) || !text(proof.ObservedAt, 64) || len(proof.Steps) < 1 || len(proof.Steps) > 64 {
		return false
	}
	var steps []map[string]json.RawMessage
	if !Decode(fields["steps"], &steps) {
		return false
	}
	names := map[string]bool{}
	for index, step := range proof.Steps {
		var ran bool
		code, err := strconv.Atoi(step.ExitCode)
		if len(steps[index]) != 6 || !Decode(steps[index]["ran"], &ran) ||
			!text(step.Name, 128) || names[step.Name] || !hex(step.CommandDigest, 64) ||
			step.Skipped == nil || step.Termination != "exited" || err != nil || code < 0 || code > 255 || strconv.Itoa(code) != step.ExitCode {
			return false
		}
		names[step.Name] = true
	}
	return true
}
