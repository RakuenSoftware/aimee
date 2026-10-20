package families

// This is a source-bound historical projection, not procedure synthesis. A
// configured test verdict qualifies only those tests on the recorded revision.
import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"sort"
	"strconv"
	"strings"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

type delegateAction struct {
	Sequence    string `json:"sequence"`
	Event       string `json:"event_id"`
	ToolID      string `json:"tool_id"`
	Namespace   string `json:"namespace,omitempty"`
	Name        string `json:"name"`
	InputDigest string `json:"input_sha256"`
	EventDigest string `json:"event_sha256"`
}
type delegateExperience struct {
	Schema          int                             `json:"schema_version"`
	Kind            string                          `json:"kind"`
	Principal       string                          `json:"principal"`
	ScopeKind       string                          `json:"scope_kind"`
	ScopeID         string                          `json:"scope_id"`
	Source          executionGradeSource            `json:"source"`
	ParentSource    executionGradeSource            `json:"parent_source"`
	Child           canonical.ChildExecutionPayload `json:"child"`
	Qualification   string                          `json:"qualification"`
	ObservedStatus  string                          `json:"observed_status"`
	VerificationRef string                          `json:"verification_ref,omitempty"`
	Verdict         string                          `json:"verdict,omitempty"`
	ActionCoverage  string                          `json:"action_coverage"`
	Actions         []delegateAction                `json:"actions"`
}

func validExperienceSource(source executionGradeSource, principal string) bool {
	sequence, err := strconv.ParseInt(source.Sequence, 10, 64)
	if err != nil || sequence <= 0 || strconv.FormatInt(sequence, 10) != source.Sequence || source.Kind != "canonical_session_log" || source.Actor != principal {
		return false
	}
	for _, s := range []string{source.Session, source.Turn, source.Event, source.Actor} {
		if s == "" || len(s) > 600 || strings.ContainsRune(s, 0) {
			return false
		}
	}
	return true
}
func loadExperienceEvent(ctx context.Context, q store.Queryer, source executionGradeSource) (string, error) {
	var raw string
	err := q.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id=$1 AND sequence::text=$2 AND turn_id=$3 AND event_id=$4 AND actor=$5`, source.Session, source.Sequence, source.Turn, source.Event, source.Actor).Scan(&raw)
	return raw, err
}
func decodeExperienceChild(raw string) (canonical.ChildExecutionPayload, bool) {
	var event struct {
		Kind    string                          `json:"kind"`
		Payload canonical.ChildExecutionPayload `json:"payload"`
	}
	if !canonical.ValidSessionEvent([]byte(raw)) || !canonical.Decode([]byte(raw), &event) || event.Kind != "child_execution" || event.Payload.Status != "running" || event.Payload.LearningBinding == nil {
		return canonical.ChildExecutionPayload{}, false
	}
	return event.Payload, true
}
func qualifiedExperienceGrade(raw string, source executionGradeSource, payload canonical.ExecutionPayload, project string) (localExecutionTestGrade, bool) {
	var grade localExecutionTestGrade
	var proof canonical.ExecutionTestProof
	if !canonical.Decode([]byte(raw), &grade) || grade.SchemaVersion != 1 || grade.Source != source || grade.ObservedStatus != "completed" || payload.Status != "completed" || grade.Qualification != "configured_tests_only" || grade.Verification != "tests_bound_revision" || (grade.Verdict != "passed" && grade.Verdict != "failed") || !executionProofInTerminal(grade.Proof, payload) || !canonical.Decode(grade.Proof, &proof) || proof.ScopeID != project || proof.ScopeKind != "project" || proof.Actor != source.Actor || proof.Session != source.Session || proof.Turn != source.Turn {
		return grade, false
	}
	return grade, true
}

type delegateExecutionSource struct {
	Parent   executionGradeSource
	Child    canonical.ChildExecutionPayload
	Source   executionGradeSource
	Event    canonical.ExecutionPayload
	Terminal string
}

func loadDelegateExecutionSource(ctx context.Context, q store.Queryer, f []string) (delegateExecutionSource, uint32, error) {
	var parent executionGradeSource
	if len(f) != 2 || len(f[1]) > 4096 || !canonical.Decode([]byte(f[1]), &parent) || !validExperienceSource(parent, f[0]) {
		return delegateExecutionSource{}, store.StatusInvalid, nil
	}
	parentRaw, err := loadExperienceEvent(ctx, q, parent)
	if err != nil {
		if store.IsNoRows(err) {
			return delegateExecutionSource{}, store.StatusMissing, nil
		}
		return delegateExecutionSource{}, store.StatusFailed, err
	}
	child, ok := decodeExperienceChild(parentRaw)
	if !ok || child.Turn != parent.Turn || child.Session == parent.Session || !validDelegateChildIdentity(parent.Session, child) {
		return delegateExecutionSource{}, store.StatusInvalid, nil
	}
	// Lock both actual owners in a stable order. Replay/concurrent publishers
	// serialize on the source sessions without a second independent authority.
	sessions := []string{parent.Session, child.Session}
	sort.Strings(sessions)
	for _, session := range sessions {
		var owner string
		if err = q.QueryRow(ctx, sessionIRLockSQL, session).Scan(&owner); err != nil {
			if store.IsNoRows(err) {
				return delegateExecutionSource{}, store.StatusMissing, nil
			}
			return delegateExecutionSource{}, store.StatusFailed, err
		}
		if owner != f[0] {
			return delegateExecutionSource{}, store.StatusInvalid, nil
		}
	}
	// Re-read after acquiring custody locks; don't use a pre-lock source snapshot.
	current, err := loadExperienceEvent(ctx, q, parent)
	if err != nil {
		return delegateExecutionSource{}, store.StatusFailed, err
	}
	if current != parentRaw {
		return delegateExecutionSource{}, store.StatusInvalid, nil
	}
	var jobRole, jobAgent string
	if err = q.QueryRow(ctx, `SELECT role,agent_name FROM agent_jobs WHERE id=$1::bigint`, child.LearningBinding.JobID).Scan(&jobRole, &jobAgent); err != nil {
		if store.IsNoRows(err) {
			return delegateExecutionSource{}, store.StatusMissing, nil
		}
		return delegateExecutionSource{}, store.StatusFailed, err
	}
	if jobRole != child.Role || jobAgent != child.LearningBinding.Agent {
		return delegateExecutionSource{}, store.StatusInvalid, nil
	}
	rows, err := q.Query(ctx, `SELECT sequence::text,event_id,payload FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND actor=$3 AND payload::jsonb->>'kind'='execution' AND payload::jsonb->'payload'->>'status'<>'running' ORDER BY sequence LIMIT 2`, child.Session, child.Turn, f[0])
	if err != nil {
		return delegateExecutionSource{}, store.StatusFailed, err
	}
	var source executionGradeSource
	var terminal string
	count := 0
	for rows.Next() {
		source = executionGradeSource{Kind: "canonical_session_log", Session: child.Session, Turn: child.Turn, Actor: f[0]}
		if err = rows.Scan(&source.Sequence, &source.Event, &terminal); err != nil {
			break
		}
		count++
	}
	rowErr := rows.Err()
	rows.Close()
	if err != nil {
		return delegateExecutionSource{}, store.StatusFailed, err
	}
	if rowErr != nil {
		return delegateExecutionSource{}, store.StatusFailed, rowErr
	}
	if count != 1 {
		return delegateExecutionSource{}, store.StatusInvalid, nil
	}
	var event struct {
		Kind    string                     `json:"kind"`
		Payload canonical.ExecutionPayload `json:"payload"`
	}
	if !canonical.ValidSessionEvent([]byte(terminal)) || !canonical.Decode([]byte(terminal), &event) || event.Kind != "execution" {
		return delegateExecutionSource{}, store.StatusInvalid, nil
	}
	return delegateExecutionSource{parent, child, source, event.Payload, terminal}, store.StatusOK, nil
}
func delegateExecutionTerminalLookup(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	record, status, err := loadDelegateExecutionSource(ctx, q, f)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	source := record.Source
	encoded, err := encodeSessionIR(map[string]any{"version": 1, "session_id": source.Session, "turn_id": source.Turn, "sequence": source.Sequence, "event_id": source.Event, "actor": source.Actor, "event": json.RawMessage(record.Terminal)})
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if len(encoded) > 2*1024*1024-1 {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{string(encoded)}, nil
}
func delegateExecutionExperienceRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	record, status, err := loadDelegateExecutionSource(ctx, q, f)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	parent, child, source := record.Parent, record.Child, record.Source
	event := struct{ Payload canonical.ExecutionPayload }{record.Event}

	var rowErr error
	experience := delegateExperience{Schema: 1, Kind: "delegate_execution_experience", Principal: f[0], ScopeKind: "project", ScopeID: child.LearningBinding.ScopeID, Source: source, ParentSource: parent, Child: child, Qualification: "ungraded", ObservedStatus: event.Payload.Status, ActionCoverage: "bounded_canonical_tool_calls", Actions: []delegateAction{}}
	grades := []localExecutionTestGrade{}
	gradeRows, err := q.Query(ctx, `SELECT payload FROM interaction_events WHERE session_id=$1 AND event_type='task_outcome' AND actor='system' AND outcome IN ('tests_passed','tests_failed') AND payload::jsonb->'source'=$2::jsonb ORDER BY id LIMIT 9`, source.Session, mustJSON(source))
	if err != nil {
		return store.StatusFailed, nil, err
	}
	for gradeRows.Next() {
		var raw string
		if err = gradeRows.Scan(&raw); err != nil {
			break
		}
		grade, valid := qualifiedExperienceGrade(raw, source, event.Payload, experience.ScopeID)
		if !valid {
			gradeRows.Close()
			return store.StatusInvalid, nil, nil
		}
		grades = append(grades, grade)
	}
	gradeErr := gradeRows.Err()
	gradeRows.Close()
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if gradeErr != nil {
		return store.StatusFailed, nil, gradeErr
	}
	if len(grades) > 8 {
		return store.StatusInvalid, nil, nil
	}
	// A retained verifier proof is authoritative evidence awaiting its derived
	// grade, never a reason to freeze this source as an ungraded observation.
	refs := map[string]bool{}
	for _, grade := range grades {
		if refs[grade.VerificationID] {
			return store.StatusInvalid, nil, nil
		}
		refs[grade.VerificationID] = true
	}
	if len(grades) != len(event.Payload.VerificationProofs) {
		return store.StatusMissing, nil, nil
	}
	rows, err := q.Query(ctx, `SELECT sequence::text,event_id,payload FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND actor=$3 AND sequence<$4::bigint AND payload::jsonb->>'kind'='message' ORDER BY sequence LIMIT 65`, source.Session, source.Turn, f[0], source.Sequence)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	for rows.Next() {
		var sequence, id, raw string
		if err = rows.Scan(&sequence, &id, &raw); err != nil {
			break
		}
		var message struct {
			Kind    string            `json:"kind"`
			Payload canonical.Message `json:"payload"`
		}
		if !canonical.ValidSessionEvent([]byte(raw)) || json.Unmarshal([]byte(raw), &message) != nil || message.Payload.Role != "assistant" {
			continue
		}
		for _, block := range message.Payload.Blocks {
			if block.Type != "tool_use" || len(experience.Actions) >= 32 {
				continue
			}
			input, _ := json.Marshal(block.Input)
			digest := sha256.Sum256(input)
			experience.Actions = append(experience.Actions, delegateAction{sequence, id, block.ToolID, block.Namespace, block.Name, hex.EncodeToString(digest[:]), eventDigest(raw)})
		}
	}
	rowErr = rows.Err()
	rows.Close()
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if rowErr != nil {
		return store.StatusFailed, nil, rowErr
	}
	if len(grades) == 0 {
		return persistDelegateExperience(ctx, q, experience)
	}
	for _, grade := range grades {
		experience.Qualification = grade.Qualification
		experience.VerificationRef = grade.VerificationID
		experience.Verdict = grade.Verdict
		status, _, err := persistDelegateExperience(ctx, q, experience)
		if err != nil || status != store.StatusOK {
			return status, nil, err
		}
	}
	return store.StatusOK, nil, nil
}
func persistDelegateExperience(ctx context.Context, q store.Queryer, experience delegateExperience) (uint32, []string, error) {
	raw := mustJSON(experience)
	source := experience.Source
	// Each derived row can recover independently on replay. Proof-bearing
	// terminals are never projected until their grades have been recovered.
	var identical bool
	err := q.QueryRow(ctx, `SELECT payload::jsonb=$5::jsonb FROM interaction_events WHERE session_id=$1 AND event_type='delegate_experience' AND payload::jsonb->'source'=$2::jsonb AND COALESCE(payload::jsonb->>'verification_ref','')=$3 AND payload::jsonb->'parent_source'=$4::jsonb ORDER BY id LIMIT 1`, source.Session, mustJSON(source), experience.VerificationRef, mustJSON(experience.ParentSource), raw).Scan(&identical)
	exists := err == nil
	if exists && !identical {
		return store.StatusInvalid, nil, nil
	}
	if err != nil && !store.IsNoRows(err) {
		return store.StatusFailed, nil, err
	}
	outcome := "observed"
	lesson := "Observed canonical child execution; task correctness and procedure causality are ungraded."
	if experience.Verdict != "" {
		outcome = "tests_" + experience.Verdict
		lesson = "Observed canonical child execution; configured tests " + experience.Verdict + " on the recorded revision only."
	}
	if !exists {
		var id int64
		if err = q.QueryRow(ctx, interactionRecordSQL, source.Session, "delegate_experience", "system", raw, outcome).Scan(&id); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	var learningExists bool
	if err = q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM delegate_learnings WHERE session_id=$1 AND evidence_json=$2)`, source.Session, raw).Scan(&learningExists); err != nil {
		return store.StatusFailed, nil, err
	}
	if !learningExists {
		if _, err = q.Exec(ctx, `INSERT INTO delegate_learnings(session_id,role,failure_mode,lesson,evidence_json,confidence,auto_applied,review_status) VALUES($1,$2,$3,$4,$5,0,0,'pending')`, source.Session, experience.Child.Role, outcome, lesson, raw); err != nil {
			return store.StatusFailed, nil, err
		}
	}
	return store.StatusOK, nil, nil
}
func mustJSON(value any) string { raw, _ := json.Marshal(value); return string(raw) }

func eventDigest(raw string) string {
	sum := sha256.Sum256([]byte(raw))
	return hex.EncodeToString(sum[:])
}

// This consistency check mirrors the actual durable producer identity. Custody
// still comes from the protected append, never from role/job number equality.
func validDelegateChildIdentity(parent string, child canonical.ChildExecutionPayload) bool {
	if child.LearningBinding == nil {
		return false
	}
	digest := sha256.Sum256([]byte(parent + ":" + child.LearningBinding.JobID + ":durable"))
	return child.Session == "delegate-"+hex.EncodeToString(digest[:])
}
