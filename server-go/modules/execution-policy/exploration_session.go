package executionpolicy

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

// SessionExploration applies a host operation to the session owner's locked
// row. The owner authenticates the caller and session before invoking it and
// commits returned state and reply in the same database transaction. It must
// never return the reply if COMMIT fails.
//
// A single session usage counter is shared by every task and delegate. Task
// snapshots retain their own revisions/attempts, not copies of that counter.
func SessionExploration(principal, session string, state, operationJSON []byte, now time.Time) ([]byte, []byte, error) {
	var req sessionExplorationRequest
	dec := json.NewDecoder(bytes.NewReader(operationJSON))
	dec.DisallowUnknownFields()
	if len(operationJSON) > 65536 || decodeSingle(dec, &req) != nil || principal == "" || session == "" || len(principal) > 128 || len(session) > 128 {
		return nil, nil, errors.New("invalid host exploration request")
	}
	if req.Operation == "check_session" || req.Operation == "bind_session" {
		var current sessionExplorationState
		if len(state) > 1048576 || json.Unmarshal(state, &current) != nil || current.Version != 1 || current.Principal != principal || current.Session != session {
			return nil, nil, errors.New("session contract unavailable")
		}
		task, ok := current.Tasks["session-task"]
		if !ok || len(task.Revisions) == 0 {
			return nil, nil, errors.New("session contract unavailable")
		}
		req.Binding = task.Revisions[len(task.Revisions)-1].Binding
		if req.Operation == "check_session" {
			req.Operation = "check"
		} else if req.Path == "" || req.Path != req.Binding.WorkingDirectory {
			return nil, nil, errors.New("execution directory differs from session contract")
		}
	}
	if !req.Binding.valid() || req.Binding.Principal != principal || req.Binding.Session != session {
		return nil, nil, errors.New("invalid authenticated task binding")
	}
	if req.Operation == "prepare" {
		req.Binding.WorktreeGeneration = explorationWorktreeGeneration(req.Binding.WorkingDirectory)
		o := req.Offer
		if o == nil || o.MemoryOwner != req.Binding.MemoryOwner || o.IndexGeneration != req.Binding.IndexGeneration {
			return nil, nil, errors.New("memory offer binding mismatch")
		}
		req.Contract = &explorationContract{ID: req.Binding.Task, Revision: 1, Binding: req.Binding, PlanDigest: o.PlanDigest, SourceVersionsDigest: o.SourceVersionsDigest, QueryClass: o.QueryClass, CoverageComplete: o.CoverageComplete, ConfidenceProvenance: o.ConfidenceProvenance, SupportedClasses: []string{"raw_scan"}, Created: now, Expires: o.Expires, Limits: explorationLimits{Enabled: false}, Tier: "observe"}
		policy, err := defaultPolicyLoader()
		if err != nil {
			return nil, nil, err
		}
		if policy != nil {
			req.Contract.Limits = policy.AdaptiveExploration
		}
		req.Operation = "issue"
	}
	var s sessionExplorationState
	if len(state) > 0 && (len(state) > 1048576 || json.Unmarshal(state, &s) != nil) {
		return nil, nil, errors.New("invalid persisted exploration state")
	}
	if s.Version == 0 && len(state) == 0 {
		s = sessionExplorationState{Version: 1, Principal: principal, Session: session, Tasks: map[string]explorationSnapshot{}}
	}
	if len(s.Groups) > 64 || len(s.Tasks) > 64 || s.Version != 1 || s.Principal != principal || s.Session != session || s.Tasks == nil {
		return nil, nil, errors.New("exploration state identity mismatch")
	}
	snapshot, exists := s.Tasks[req.Binding.Task]
	if !exists && req.Operation != "issue" {
		return nil, nil, errors.New("task contract unavailable")
	}
	if !exists && len(s.Tasks) >= 64 {
		return nil, nil, errors.New("session task history full")
	}
	if exists && (len(snapshot.Revisions) == 0 || len(snapshot.Revisions) > 128) {
		return nil, nil, errors.New("invalid task revision history")
	}
	if exists && req.Operation != "issue" && req.Operation != "check" && req.Operation != "reserve" && snapshot.Revisions[len(snapshot.Revisions)-1].Binding != req.Binding {
		return nil, nil, errors.New("task binding changed; refresh required")
	}
	// Older rows have independent task counters. Upgrade them lazily without
	// dropping spent allowance; new child jobs share their inherited root key.
	if s.Groups == nil {
		s.Groups = make(map[string]explorationUsage, len(s.Tasks))
		for key, task := range s.Tasks {
			s.Groups[key] = cloneUsage(task.TaskUsage)
		}
	}
	group := req.Binding.budgetTask()
	if exists {
		group = snapshot.Revisions[len(snapshot.Revisions)-1].Binding.budgetTask()
	}
	snapshot.TaskUsage = cloneUsage(s.Groups[group])
	snapshot.SessionUsage = cloneUsage(s.Usage)
	l := newExplorationLedger(snapshot, func(explorationSnapshot) error { return nil })
	var result any = map[string]any{"status": "ok"}
	var err error
	switch req.Operation {
	case "issue":
		if req.Contract == nil || req.Contract.Binding != req.Binding {
			return nil, nil, errors.New("contract binding mismatch")
		}
		if exists {
			old := snapshot.Revisions[len(snapshot.Revisions)-1]
			if old.ID == req.Contract.ID && old.PlanDigest == req.Contract.PlanDigest && old.SourceVersionsDigest == req.Contract.SourceVersionsDigest && old.Binding == req.Contract.Binding && old.valid(now) {
				result = map[string]any{"status": "ok", "contract": old}
				break
			}
			req.Contract.ID = old.ID
			req.Contract.Revision = old.Revision + 1
		} else {
			req.Contract.Revision = 1
		}
		// Activation is unavailable without a separately verified calibration
		// artifact. A host/model similarity score cannot turn this into enforcement.
		req.Contract.Tier = "observe"
		err = l.issue(*req.Contract, now)
		result = map[string]any{"status": "ok", "contract": req.Contract}
	case "check":
		policy, loadErr := defaultPolicyLoader()
		if loadErr != nil {
			return nil, nil, loadErr
		}
		baseline := evaluate(request{Tool: req.Tool, SideEffect: req.SideEffect, Arguments: req.Arguments}, policy)
		result = baseline
		if !baseline.Allowed || baseline.Exploration == nil {
			break
		}
		operator := explorationLimits{}
		if policy != nil {
			operator = policy.Exploration
		}
		if req.UnknownOutput && (operator.Bytes != nil || operator.Tokens != nil) {
			return nil, nil, errors.New("operator output allowance requires a host-enforced output bound")
		}
		var canonical any
		if json.Unmarshal(req.Arguments, &canonical) != nil {
			return nil, nil, errors.New("invalid arguments")
		}
		action, _ := json.Marshal([]any{req.Tool, canonical})
		// A changed or dirty worktree invalidates only the adaptive binding.
		// The same durable root/session counters still enforce operator ceilings.
		if strings.HasPrefix(req.Binding.WorktreeGeneration, "git-clean:") {
			req.Binding.WorktreeGeneration = explorationWorktreeGeneration(req.Binding.WorkingDirectory)
		}
		a := explorationAttempt{ID: req.AttemptID, Binding: req.Binding, Class: "raw_scan", Path: explorationDiscoveryPath(req.Tool, req.Arguments, req.Binding.WorkingDirectory), Bytes: req.Bytes, Tokens: req.Tokens, ActionDigest: fmt.Sprintf("%x", sha256.Sum256(action))}
		var decision explorationDecision
		decision, err = l.reserve(a, operator, false, nil, now)
		// This check is the host's dispatch admission, after hard directives.
		// Commit admission and possible-dispatch together: a lost response or
		// hook client crash cannot leave a refundable, possibly executed call.
		if err == nil && !decision.Restricted {
			err = l.started(a.ID)
		}
		baseline.Exploration = &decision
		if decision.Restricted {
			baseline.Allowed = false
			baseline.Reason = decision.Reason
		}
		result = baseline
	case "reserve":
		if req.Attempt == nil || req.Attempt.Binding != req.Binding {
			return nil, nil, errors.New("attempt binding mismatch")
		}
		policy, loadErr := defaultPolicyLoader()
		if loadErr != nil {
			return nil, nil, loadErr
		}
		operator := explorationLimits{}
		if policy != nil {
			operator = policy.Exploration
		}
		result, err = l.reserve(*req.Attempt, operator, false, nil, now)
	case "observe_indexed":
		if req.Tool != "code_search" && req.Tool != "find_symbol" {
			return nil, nil, errors.New("unsupported indexed outcome")
		}
		var arguments map[string]any
		if json.Unmarshal(req.Arguments, &arguments) != nil || arguments == nil {
			return nil, nil, errors.New("invalid indexed observation arguments")
		}
		if project := textField(arguments, "project"); project != "" && project != req.Binding.Project {
			return nil, nil, errors.New("indexed outcome is outside this task project")
		}
		text := strings.TrimSpace(req.ToolResult)
		status := ""
		// Match the real native adapters' empty-result formats exactly. Text
		// containing an empty-result phrase alongside actual hits is not empty.
		symbol := textField(arguments, "identifier")
		if (req.Tool == "code_search" && text == "[]") ||
			(req.Tool == "find_symbol" && symbol != "" && text == "No symbol found for '"+symbol+"'") {
			status = "empty"
		} else if strings.HasPrefix(text, "error:") {
			status = "failed"
		}
		if status == "" {
			break
		}
		if len(req.ToolResult) > 4096 || req.AttemptID == "" {
			return nil, nil, errors.New("invalid indexed result observation")
		}
		c := snapshot.Revisions[len(snapshot.Revisions)-1]
		digest := sha256.Sum256(append([]byte(req.Tool+":"), req.Arguments...))
		gap := fmt.Sprintf("indexed:%x", digest[:16])
		outcome := explorationIndexedOutcome{ID: req.AttemptID, Binding: req.Binding, ContractID: c.ID, Revision: c.Revision, Gap: gap, Class: "raw_scan", Path: req.Binding.WorkingDirectory, Status: status, Completed: now}
		if previous, ok := snapshot.Outcomes[req.AttemptID]; ok {
			if previous.Binding != outcome.Binding || previous.Gap != outcome.Gap || previous.Status != outcome.Status {
				return nil, nil, errors.New("indexed observation identity changed")
			}
			result = map[string]any{"status": "ok", "gap_ref": previous.Gap, "outcome_id": previous.ID, "expansion_available": true}
			break
		} else {
			l.state.UnresolvedGap = true
		}
		err = l.recordIndexedOutcome(outcome, now)
		result = map[string]any{"status": "ok", "gap_ref": gap, "outcome_id": req.AttemptID, "expansion_available": err == nil}
	case "observe_turn":
		if req.TurnID == "" || len(req.TurnID) > 256 {
			return nil, nil, errors.New("invalid completed turn identity")
		}
		if l.state.CompletedTurns[req.TurnID] {
			break
		}
		if len(l.state.CompletedTurns) >= 256 {
			return nil, nil, errors.New("completed turn history full")
		}
		constrained, failed := false, false
		for _, attempt := range snapshot.Attempts {
			if attempt.ReservedAt.After(snapshot.CompletedAt) && attempt.Decision.WouldRestrict {
				constrained = true
			}
		}
		for _, outcome := range snapshot.Outcomes {
			if outcome.Completed.After(snapshot.CompletedAt) {
				failed = true
			}
		}
		err = l.completedTurn(snapshot.LastTurn+1, constrained, snapshot.UnresolvedGap, failed, false)
		if err == nil {
			if l.state.CompletedTurns == nil {
				l.state.CompletedTurns = map[string]bool{}
			}
			l.state.CompletedTurns[req.TurnID] = true
			l.state.CompletedAt = now
		}
	case "started":
		err = l.started(req.AttemptID)
	case "cancel_before_dispatch":
		err = l.cancelBeforeDispatch(req.AttemptID)
	case "indexed_outcome":
		if req.Outcome == nil || req.Outcome.Binding != req.Binding {
			return nil, nil, errors.New("outcome binding mismatch")
		}
		err = l.recordIndexedOutcome(*req.Outcome, now)
	case "expand":
		err = l.expand(req.Reason, req.Gap, req.OutcomeID, now)
	case "completed_turn":
		err = l.completedTurn(req.Turn, req.Constrained, req.Unresolved, req.IndexedFailed, req.VerifiedProgress)
	case "bind_session":
		result = map[string]any{"status": "ok", "binding": req.Binding}
	case "inspect":
		result = map[string]any{"status": "ok", "contract": snapshot.Revisions[len(snapshot.Revisions)-1], "task_usage": snapshot.TaskUsage, "session_usage": s.Usage}
	default:
		return nil, nil, errors.New("unknown exploration operation")
	}
	if err != nil {
		return nil, nil, err
	}
	s.Usage = cloneUsage(l.state.SessionUsage)
	s.Groups[group] = cloneUsage(l.state.TaskUsage)
	next := l.state.clone()
	next.SessionUsage = explorationUsage{}
	s.Tasks[req.Binding.Task] = next
	encoded, err := json.Marshal(s)
	if err != nil || len(encoded) > 1048576 {
		return nil, nil, errors.New("session exploration state capacity exceeded")
	}
	reply, err := json.Marshal(result)
	return encoded, reply, err
}

type sessionExplorationState struct {
	Groups    map[string]explorationUsage    `json:"budget_groups,omitempty"`
	Version   int                            `json:"version"`
	Principal string                         `json:"principal"`
	Session   string                         `json:"session"`
	Usage     explorationUsage               `json:"usage"`
	Tasks     map[string]explorationSnapshot `json:"tasks"`
}
type sessionExplorationRequest struct {
	TurnID           string                     `json:"turn_id,omitempty"`
	ToolResult       string                     `json:"tool_result,omitempty"`
	UnknownOutput    bool                       `json:"unknown_output,omitempty"`
	Tool             string                     `json:"tool,omitempty"`
	SideEffect       string                     `json:"side_effect,omitempty"`
	Arguments        json.RawMessage            `json:"tool_arguments,omitempty"`
	Path             string                     `json:"canonical_path,omitempty"`
	Bytes            int64                      `json:"bytes,omitempty"`
	Tokens           int64                      `json:"tokens,omitempty"`
	Offer            *sessionExplorationOffer   `json:"offer,omitempty"`
	Operation        string                     `json:"operation"`
	Binding          explorationBinding         `json:"binding"`
	Contract         *explorationContract       `json:"contract,omitempty"`
	Attempt          *explorationAttempt        `json:"attempt,omitempty"`
	Outcome          *explorationIndexedOutcome `json:"outcome,omitempty"`
	AttemptID        string                     `json:"attempt_id,omitempty"`
	OutcomeID        string                     `json:"outcome_id,omitempty"`
	Reason           string                     `json:"reason,omitempty"`
	Gap              string                     `json:"gap,omitempty"`
	Turn             uint64                     `json:"turn,omitempty"`
	Constrained      bool                       `json:"constrained,omitempty"`
	Unresolved       bool                       `json:"unresolved,omitempty"`
	IndexedFailed    bool                       `json:"indexed_failed,omitempty"`
	VerifiedProgress bool                       `json:"verified_progress,omitempty"`
}

type sessionExplorationOffer struct {
	MemoryOwner          string    `json:"memory_owner"`
	PlanDigest           string    `json:"plan_digest"`
	SourceVersionsDigest string    `json:"source_versions_digest"`
	QueryClass           string    `json:"query_class"`
	CoverageComplete     bool      `json:"coverage_complete"`
	ConfidenceProvenance string    `json:"confidence_provenance"`
	IndexGeneration      string    `json:"index_generation"`
	Expires              time.Time `json:"expires"`
}
