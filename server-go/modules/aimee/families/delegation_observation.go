package families

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"sort"
	"strconv"
	"strings"
)

// This exact expression is indexed by migration 52. Invalid historical JSON
// cannot abort an owner-scoped observation or be interpreted as a binding.
const agentJobBindingExpression = `(CASE WHEN payload IS JSON OBJECT WITH UNIQUE KEYS THEN CASE WHEN payload::jsonb->>'kind'='child_execution' AND payload::jsonb#>>'{payload,status}'='running' THEN payload::jsonb#>>'{payload,learning_binding,job_id}' END END)`
const agentJobBindingSQL = `SELECT session_id,sequence::text,event_id,turn_id,actor,payload FROM session_ir_events WHERE actor=$2 AND ` + agentJobBindingExpression + `=$1 AND EXISTS(SELECT 1 FROM server_sessions s WHERE s.id=session_id AND s.principal=$2) ORDER BY session_id,sequence LIMIT 129`
const agentJobObservationMaxBytes = 8 << 20

var errAgentJobBindingInvalid = errors.New("invalid canonical delegate source binding")

type agentJobObservationEvent struct {
	Sequence string          `json:"sequence"`
	EventID  string          `json:"event_id"`
	TurnID   string          `json:"turn_id"`
	Actor    string          `json:"actor"`
	Payload  json.RawMessage `json:"payload"`
}
type agentJobObservationChild struct {
	Session string `json:"session_id"`
	Turn    string `json:"turn_id"`
	Role    string `json:"role"`
	Agent   string `json:"agent"`
}
type agentJobObservation struct {
	Schema       string                     `json:"schema"`
	JobID        string                     `json:"job_id"`
	Parent       executionGradeSource       `json:"parent_source"`
	Child        agentJobObservationChild   `json:"child"`
	LastSequence string                     `json:"last_sequence"`
	Status       string                     `json:"execution_status"`
	Terminal     *executionGradeSource      `json:"terminal_source"`
	History      json.RawMessage            `json:"history"`
	Events       []agentJobObservationEvent `json:"events"`
}
type agentJobBinding struct {
	source executionGradeSource
	child  canonical.ChildExecutionPayload
	raw    string
}

func agentJobBindings(ctx context.Context, q store.Queryer, job, actor string) ([]agentJobBinding, error) {
	rows, err := q.Query(ctx, agentJobBindingSQL, job, actor)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []agentJobBinding
	sourceBytes := 0
	for rows.Next() {
		source := executionGradeSource{Kind: "canonical_session_log"}
		var raw string
		if err = rows.Scan(&source.Session, &source.Sequence, &source.Event, &source.Turn, &source.Actor, &raw); err != nil {
			return nil, err
		}
		child, ok := decodeExperienceChild(raw)
		if !ok || !validExperienceSource(source, actor) || child.LearningBinding.JobID != job || child.Turn != source.Turn || child.Session == source.Session || !validDelegateChildIdentity(source.Session, child) {
			return nil, errAgentJobBindingInvalid
		}
		if len(result) > 0 && (result[0].source.Session != source.Session || result[0].child.Session != child.Session) {
			return nil, errAgentJobBindingInvalid
		}
		sourceBytes += len(raw)
		if sourceBytes > agentJobObservationMaxBytes {
			return nil, errAgentJobBindingInvalid
		}
		if len(result) >= 128 {
			return nil, errAgentJobBindingInvalid
		}
		result = append(result, agentJobBinding{source, child, raw})
	}
	return result, rows.Err()
}
func agentJobCanonicalObservation(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	job, err := strconv.ParseInt(f[0], 10, 32)
	if err != nil || job <= 0 || strconv.FormatInt(job, 10) != f[0] || f[1] == "" || len(f[1]) > 1024 || strings.ContainsRune(f[1], 0) {
		return store.StatusInvalid, nil, nil
	}
	bindings, err := agentJobBindings(ctx, q, f[0], f[1])
	if err != nil {
		if errors.Is(err, errAgentJobBindingInvalid) {
			return store.StatusInvalid, nil, nil
		}
		return 0, nil, err
	}
	if len(bindings) == 0 {
		// Denial reveals no source data. The same job-leading index answers the
		// existence check without scanning another actor's history.
		var exists bool
		if err = q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM session_ir_events WHERE `+agentJobBindingExpression+`=$1)`, f[0]).Scan(&exists); err != nil {
			return 0, nil, err
		}
		if exists {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusMissing, nil, nil
	}
	selected := bindings[len(bindings)-1]
	sessions := []string{selected.source.Session, selected.child.Session}
	sort.Strings(sessions)
	for _, session := range sessions {
		var owner string
		if err = q.QueryRow(ctx, sessionIRLockSQL, session).Scan(&owner); err != nil {
			if store.IsNoRows(err) {
				return store.StatusInvalid, nil, nil
			}
			return 0, nil, err
		}
		if owner != f[1] {
			return store.StatusInvalid, nil, nil
		}
	}
	// Re-read the binding after taking the same custody locks as append/history.
	current, err := agentJobBindings(ctx, q, f[0], f[1])
	if err != nil {
		if errors.Is(err, errAgentJobBindingInvalid) {
			return store.StatusInvalid, nil, nil
		}
		return 0, nil, err
	}
	if len(current) != len(bindings) || len(current) == 0 || current[len(current)-1].raw != selected.raw || current[len(current)-1].source != selected.source {
		return store.StatusInvalid, nil, nil
	}
	var role, agent string
	if err = q.QueryRow(ctx, `SELECT role,agent_name FROM agent_jobs WHERE id=$1::bigint FOR SHARE`, f[0]).Scan(&role, &agent); err != nil {
		if store.IsNoRows(err) {
			return store.StatusInvalid, nil, nil
		}
		return 0, nil, err
	}
	if role != selected.child.Role || agent != selected.child.LearningBinding.Agent {
		return store.StatusInvalid, nil, nil
	}
	observation := agentJobObservation{Schema: "aimee.agent-job-observation.v1", JobID: f[0], Parent: selected.source, Child: agentJobObservationChild{selected.child.Session, selected.child.Turn, role, agent}, Events: []agentJobObservationEvent{}}
	first, err := q.Query(ctx, sessionIRHistorySQL, selected.child.Session, f[1], "0")
	if err != nil {
		return 0, nil, err
	}
	rows := &sessionIRHistoryRows{ctx: ctx, q: q, session: selected.child.Session, actor: f[1], after: "0", rows: first}
	defer rows.Close()
	var previous uint64
	var terminalCount, startCount, total int
	for rows.Next() {
		var event agentJobObservationEvent
		var raw string
		if err = rows.Scan(&event.Sequence, &event.TurnID, &event.Actor, &event.EventID, &raw); err != nil {
			return 0, nil, err
		}
		sequence, e := strconv.ParseUint(event.Sequence, 10, 64)
		if e != nil || sequence != previous+1 || strconv.FormatUint(sequence, 10) != event.Sequence || event.Actor != f[1] || event.EventID == "" || !canonical.ValidSessionEvent([]byte(raw)) {
			return store.StatusInvalid, nil, nil
		}
		previous = sequence
		event.Payload = json.RawMessage(raw)
		total += len(raw) + len(event.EventID) + len(event.TurnID) + len(event.Actor) + 128
		if total > agentJobObservationMaxBytes {
			return store.StatusInvalid, nil, fmt.Errorf("canonical delegate observation exceeds reply budget")
		}
		observation.Events = append(observation.Events, event)
		observation.LastSequence = event.Sequence
		var envelope struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ExecutionPayload `json:"payload"`
		}
		// Decode only the execution envelope; other typed events retain exact bytes.
		var kind struct {
			Kind string `json:"kind"`
		}
		if json.Unmarshal([]byte(raw), &kind) != nil {
			return store.StatusInvalid, nil, nil
		}
		if event.TurnID == selected.child.Turn && kind.Kind == "execution" {
			if !canonical.Decode([]byte(raw), &envelope) {
				return store.StatusInvalid, nil, nil
			}
			switch envelope.Payload.Status {
			case "running":
				startCount++
			case "completed", "failed", "cancelled":
				terminalCount++
				source := executionGradeSource{Kind: "canonical_session_log", Session: selected.child.Session, Turn: event.TurnID, Sequence: event.Sequence, Event: event.EventID, Actor: event.Actor}
				observation.Terminal = &source
			default:
				return store.StatusInvalid, nil, nil
			}
			observation.Status = envelope.Payload.Status
		}
	}
	if err = rows.Err(); err != nil {
		return 0, nil, err
	}
	rows.Close()
	if startCount == 0 {
		if len(observation.Events) > 0 {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusMissing, nil, nil
	}
	if startCount != 1 || terminalCount > 1 || observation.Status == "" || (terminalCount == 1 && observation.Status == "running") {
		return store.StatusInvalid, nil, nil
	}
	var status uint32
	var history []string
	if observation.Status == "running" {
		status, history, err = sessionIRHistoryProject(ctx, q, []string{selected.child.Session, "primary", f[1], selected.child.Turn}, true)
	} else {
		status, history, err = sessionIRHistoryProject(ctx, q, []string{selected.child.Session, "primary", f[1], ""}, false)
	}
	if err == nil && status == store.StatusMissing {
		// An actual execution can fail before its first model message. Preserve
		// its source and receipts; an empty history does not invent a response.
		status = store.StatusOK
		history = []string{"[]"}
	}
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(history) != 1 {
		return store.StatusInvalid, nil, nil
	}
	observation.History = json.RawMessage(history[0])
	encoded, err := encodeSessionIR(observation)
	if err != nil {
		return 0, nil, err
	}
	if len(encoded) > agentJobObservationMaxBytes {
		return store.StatusInvalid, nil, fmt.Errorf("canonical delegate observation exceeds reply budget")
	}
	return store.StatusOK, []string{string(encoded)}, nil
}
