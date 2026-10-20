package families

// Recall is a scoped projection of immutable, independently admitted test
// receipts. It is evidence about configured tests, never a procedure or a claim
// that the model solved the user's task.
import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const executionGradeRecallSQL = `SELECT i.payload,e.payload,
 COALESCE((SELECT jsonb_build_object('experience',d.payload::jsonb,'parent',p.payload::jsonb)::text
 FROM interaction_events d
 JOIN server_sessions ps ON ps.id=d.payload::jsonb->'parent_source'->>'session_id' AND ps.principal=$1
 JOIN session_ir_events p ON p.session_id=ps.id AND p.actor=$1
 AND p.sequence::text=d.payload::jsonb->'parent_source'->>'sequence'
 AND p.event_id=d.payload::jsonb->'parent_source'->>'event_id'
 AND p.turn_id=d.payload::jsonb->'parent_source'->>'turn_id'
 JOIN agent_jobs j ON j.id::text=d.payload::jsonb->'child'->'learning_binding'->>'job_id'
 AND j.role=d.payload::jsonb->'child'->>'role'
 AND j.agent_name=d.payload::jsonb->'child'->'learning_binding'->>'agent'
 WHERE d.session_id=i.session_id AND d.event_type='delegate_experience' AND d.actor='system'
 AND d.payload::jsonb->'source'=i.payload::jsonb->'source'
 AND d.payload::jsonb->>'verification_ref'=i.payload::jsonb->>'verification_ref'
 AND d.payload::jsonb->>'principal'=$1 AND d.payload::jsonb->>'scope_id'=$2
 AND NOT EXISTS (SELECT 1 FROM jsonb_array_elements(d.payload::jsonb->'actions') action
 WHERE NOT EXISTS(SELECT 1 FROM session_ir_events ae WHERE ae.session_id=i.session_id
 AND ae.actor=$1 AND ae.turn_id=e.turn_id AND ae.sequence::text=action->>'sequence'
 AND ae.event_id=action->>'event_id'
 AND encode(sha256(convert_to(ae.payload,'UTF8')),'hex')=action->>'event_sha256'))
 ORDER BY d.id DESC LIMIT 1),'null')
 FROM interaction_events i
 JOIN server_sessions s ON s.id=i.session_id AND s.principal=$1
 JOIN session_ir_events e ON e.session_id=i.session_id
 AND e.sequence::text=i.payload::jsonb->'source'->>'sequence'
 AND e.event_id=i.payload::jsonb->'source'->>'event_id'
 AND e.turn_id=i.payload::jsonb->'source'->>'turn_id' AND e.actor=$1
 WHERE i.event_type='task_outcome' AND i.actor='system'
 AND i.outcome IN ('tests_passed','tests_failed')
 AND i.payload::jsonb->'source'->>'actor'=$1
 AND i.payload::jsonb->'source'->>'session_id'=i.session_id
 AND i.payload::jsonb->'proof'->>'scope_kind'='project'
 AND i.payload::jsonb->'proof'->>'scope_id'=$2
 AND ($3::jsonb IS NULL OR EXISTS (SELECT 1 FROM jsonb_array_elements($3::jsonb->'items') wanted
 WHERE wanted->'source'=i.payload::jsonb->'source' AND wanted->>'verification_ref'=i.payload::jsonb->>'verification_ref'))
 ORDER BY i.id DESC LIMIT 64`

func executionTestGradeRecall(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	for n, v := range f[:2] {
		limit := 600
		if n == 1 {
			limit = 256
		}
		if v == "" || len(v) > limit || strings.ContainsRune(v, 0) {
			return store.StatusInvalid, nil, nil
		}
	}
	var wanted any
	maxItems := 8
	checkID := ""
	if len(f) > 2 && f[2] != "" {
		var snapshot struct {
			Schema    int               `json:"schema_version"`
			CheckID   string            `json:"check_id"`
			Principal string            `json:"principal"`
			ScopeKind string            `json:"scope_kind"`
			ScopeID   string            `json:"scope_id"`
			Items     []json.RawMessage `json:"items"`
		}
		if len(f[2]) > 65535 || json.Unmarshal([]byte(f[2]), &snapshot) != nil || snapshot.Schema != 1 || snapshot.Principal != f[0] || snapshot.ScopeKind != "project" || snapshot.ScopeID != f[1] || len(snapshot.Items) > 64 {
			return store.StatusInvalid, nil, nil
		}
		var check struct {
			ID string `json:"check_id"`
		}
		if json.Unmarshal([]byte(f[2]), &check) != nil || (check.ID != "" && len(check.ID) != 32) {
			return store.StatusInvalid, nil, nil
		}
		checkID = check.ID
		wanted = f[2]
		maxItems = 64
	}
	rows, err := q.Query(ctx, executionGradeRecallSQL, f[0], f[1], wanted)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	defer rows.Close()
	items := []localExecutionTestGrade{}
	total := 0
	for rows.Next() {
		var raw, terminal, experienceRaw string
		if err = rows.Scan(&raw, &terminal, &experienceRaw); err != nil {
			return store.StatusFailed, nil, err
		}
		if len(raw) > 256*1024 || !canonical.ValidSessionEvent([]byte(terminal)) {
			continue
		}
		var event struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ExecutionPayload `json:"payload"`
		}
		var grade localExecutionTestGrade
		var binding struct {
			Version   int    `json:"version"`
			Actor     string `json:"actor"`
			Session   string `json:"session_id"`
			Turn      string `json:"turn_id"`
			ScopeKind string `json:"scope_kind"`
			ScopeID   string `json:"scope_id"`
		}
		if json.Unmarshal([]byte(terminal), &event) != nil || event.Kind != "execution" || event.Payload.Status != "completed" || !canonical.Decode([]byte(raw), &grade) || json.Unmarshal(grade.Proof, &binding) != nil {
			continue
		}
		if !executionProofInTerminal(grade.Proof, event.Payload) {
			continue
		}
		if grade.SchemaVersion != 1 || grade.Source.Kind != "canonical_session_log" || grade.Source.Actor != f[0] || grade.ObservedStatus != "completed" || grade.Verification != "tests_bound_revision" || grade.Qualification != "configured_tests_only" || (grade.Verdict != "passed" && grade.Verdict != "failed") || binding.Version != 1 || binding.Actor != f[0] || binding.Session != grade.Source.Session || binding.Turn != grade.Source.Turn || binding.ScopeKind != "project" || binding.ScopeID != f[1] {
			continue
		}
		if experienceRaw != "null" {
			var retained struct {
				Experience delegateExperience `json:"experience"`
				Parent     json.RawMessage    `json:"parent"`
			}
			if json.Unmarshal([]byte(experienceRaw), &retained) == nil {
				exp := retained.Experience
				child, valid := decodeExperienceChild(string(retained.Parent))
				if valid && exp.Schema == 1 && exp.Kind == "delegate_execution_experience" && exp.Principal == f[0] && exp.ScopeKind == "project" && exp.ScopeID == f[1] && exp.Source == grade.Source && validExperienceSource(exp.ParentSource, f[0]) && exp.VerificationRef == grade.VerificationID && exp.Verdict == grade.Verdict && exp.Qualification == "configured_tests_only" && exp.ObservedStatus == "completed" && child.Session == grade.Source.Session && child.Turn == grade.Source.Turn && child.LearningBinding.ScopeID == f[1] && mustJSON(child) == mustJSON(exp.Child) {
					grade.Experience = &exp
				}
			}
		}
		// Context carries a typed receipt projection, not arbitrary verifier
		// output. Bind the complete stored body by digest and retain only the
		// revision/configuration/profile identifiers needed to interpret it.
		var fullProof map[string]json.RawMessage
		if json.Unmarshal(grade.Proof, &fullProof) != nil {
			continue
		}
		projection := map[string]any{"scope_kind": "project", "scope_id": f[1]}
		for _, key := range []string{"snapshot_commit", "snapshot_tree", "configuration_sha256", "profile_sha256", "observed_at"} {
			if value, ok := fullProof[key]; ok {
				projection[key] = value
			}
		}
		checksum := sha256.Sum256([]byte(raw))
		projection["receipt_sha256"] = hex.EncodeToString(checksum[:])
		grade.Proof, _ = json.Marshal(projection)
		encoded, err := json.Marshal(grade)
		if err != nil {
			return store.StatusFailed, nil, err
		}
		// Whole records only: a too-large receipt cannot displace smaller later ones.
		if len(items) >= maxItems || total+len(encoded) > 60*1024 {
			continue
		}
		total += len(encoded)
		items = append(items, grade)
	}
	if err = rows.Err(); err != nil {
		return store.StatusFailed, nil, err
	}
	snapshot := map[string]any{"schema_version": 1, "principal": f[0], "scope_kind": "project", "scope_id": f[1], "coverage": "recent_bounded", "items": items}
	if checkID != "" {
		snapshot["check_id"] = checkID
	}
	raw, err := json.Marshal(snapshot)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, []string{string(raw)}, nil
}
