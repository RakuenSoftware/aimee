package families

// Storage integrity only. Learning owns verification policy; this private
// publisher binds its admitted result to the immutable canonical terminal.
import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"strings"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

type executionGradeSource struct {
	Kind     string `json:"kind"`
	Session  string `json:"session_id"`
	Turn     string `json:"turn_id"`
	Sequence string `json:"sequence"`
	Event    string `json:"event_id"`
	Actor    string `json:"actor"`
}

type localExecutionTestGrade struct {
	SchemaVersion  int                  `json:"schema_version"`
	Source         executionGradeSource `json:"source"`
	ObservedStatus string               `json:"observed_status"`
	Verification   string               `json:"verification"`
	Qualification  string               `json:"qualification"`
	Verdict        string               `json:"verdict"`
	VerificationID string               `json:"verification_ref"`
	Proof          json.RawMessage      `json:"proof"`
	Experience     *delegateExperience  `json:"delegate_experience,omitempty"`
}

// Match the complete admitted typed proof, independent of JSON member order
// and whitespace. This is source integrity only, not execution qualification.
func executionProofInTerminal(encoded []byte, payload canonical.ExecutionPayload) bool {
	if !canonical.ValidVerificationProof(encoded) {
		return false
	}
	var requested canonical.ExecutionTestProof
	if !canonical.Decode(encoded, &requested) {
		return false
	}
	canonicalRequest, err := json.Marshal(requested)
	if err != nil {
		return false
	}
	for _, proof := range payload.VerificationProofs {
		canonicalSource, err := json.Marshal(proof)
		if err == nil && bytes.Equal(canonicalRequest, canonicalSource) {
			return true
		}
	}
	return false
}

func executionTestGradeRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	var grade localExecutionTestGrade
	if f[0] == "" || len(f[0]) > 600 || len(f[1]) > 256*1024 ||
		!canonical.Decode([]byte(f[1]), &grade) || grade.Experience != nil || grade.SchemaVersion != 1 ||
		grade.ObservedStatus != "completed" || grade.Verification != "tests_bound_revision" ||
		grade.Qualification != "configured_tests_only" || (grade.Verdict != "passed" && grade.Verdict != "failed") ||
		!strings.HasPrefix(grade.VerificationID, "sha256:") || len(grade.VerificationID) != 71 ||
		strings.ToLower(grade.VerificationID) != grade.VerificationID {
		return store.StatusInvalid, nil, nil
	}
	if _, err := hex.DecodeString(strings.TrimPrefix(grade.VerificationID, "sha256:")); err != nil {
		return store.StatusInvalid, nil, nil
	}
	source := grade.Source
	sequence, err := strconv.ParseInt(source.Sequence, 10, 64)
	if err != nil || sequence <= 0 || strconv.FormatInt(sequence, 10) != source.Sequence ||
		source.Kind != "canonical_session_log" || source.Actor != f[0] {
		return store.StatusInvalid, nil, nil
	}
	for _, identity := range []string{source.Session, source.Turn, source.Event, source.Actor} {
		if identity == "" || len(identity) > 600 || strings.ContainsRune(identity, 0) {
			return store.StatusInvalid, nil, nil
		}
	}
	// The proof body remains owned by Learning. Storage checks only its binding;
	// it does not reinterpret exit codes or manufacture a verifier verdict.
	var binding struct {
		Version   int    `json:"version"`
		Session   string `json:"session_id"`
		Turn      string `json:"turn_id"`
		Actor     string `json:"actor"`
		ScopeKind string `json:"scope_kind"`
		ScopeID   string `json:"scope_id"`
	}
	if json.Unmarshal(grade.Proof, &binding) != nil || binding.Version != 1 ||
		binding.Session != source.Session || binding.Turn != source.Turn || binding.Actor != f[0] ||
		binding.ScopeKind != "project" || binding.ScopeID == "" || len(binding.ScopeID) > 256 ||
		strings.ContainsRune(binding.ScopeID, 0) {
		return store.StatusInvalid, nil, nil
	}
	var owner string
	if err = q.QueryRow(ctx, sessionIRLockSQL, source.Session).Scan(&owner); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return store.StatusFailed, nil, err
	}
	if owner != f[0] {
		return store.StatusInvalid, nil, nil
	}
	var payload string
	err = q.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id=$1 AND sequence=$2 AND event_id=$3 AND turn_id=$4 AND actor=$5`,
		source.Session, sequence, source.Event, source.Turn, source.Actor).Scan(&payload)
	if err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return store.StatusFailed, nil, err
	}
	var terminal struct {
		Kind    string                     `json:"kind"`
		Payload canonical.ExecutionPayload `json:"payload"`
	}
	if !canonical.ValidSessionEvent([]byte(payload)) || json.Unmarshal([]byte(payload), &terminal) != nil ||
		terminal.Kind != "execution" || terminal.Payload.Status != "completed" || !executionProofInTerminal(grade.Proof, terminal.Payload) {
		return store.StatusInvalid, nil, nil
	}
	var identical bool
	err = q.QueryRow(ctx, `SELECT payload::jsonb=$4::jsonb FROM interaction_events WHERE session_id=$1 AND event_type='task_outcome' AND outcome IN ('tests_passed','tests_failed') AND payload::jsonb->'source'->>'sequence'=$2 AND payload::jsonb->>'verification_ref'=$3 ORDER BY id LIMIT 1`,
		source.Session, source.Sequence, grade.VerificationID, f[1]).Scan(&identical)
	if err == nil {
		if !identical {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, nil, nil
	}
	if !store.IsNoRows(err) {
		return store.StatusFailed, nil, err
	}
	var id int64
	if err = q.QueryRow(ctx, interactionRecordSQL, source.Session, "task_outcome", "system", f[1], "tests_"+grade.Verdict).Scan(&id); err != nil {
		return store.StatusFailed, nil, err
	}
	if _, err = q.Exec(ctx, interactionEvictSQL, interactionTableCap); err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}
