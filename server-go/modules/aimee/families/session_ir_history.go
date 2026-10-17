package families

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Checkpoints are host projections of admitted/executed IR, not model output.
// Observation events remain immutable, including calls removed by governance.
// The checkpoint and every message are appended in one owner-checked transaction.
type irHistoryEvent struct {
	Scope   string          `json:"scope"`
	Phase   string          `json:"phase"`
	Index   int             `json:"index,omitempty"`
	Count   int             `json:"count,omitempty"`
	Message json.RawMessage `json:"message,omitempty"`
}

func validIRHistoryMessage(raw json.RawMessage) bool {
	var message struct {
		Role     string            `json:"role"`
		Blocks   []json.RawMessage `json:"blocks"`
		Boundary bool              `json:"_compaction_boundary,omitempty"`
	}
	if !decodeIR(raw, &message) || message.Role == "" || message.Blocks == nil || len(message.Blocks) > 4096 {
		return false
	}
	for _, block := range message.Blocks {
		if !validIRBlock(block) {
			return false
		}
	}
	return true
}

func validIRHistoryEvent(raw json.RawMessage) bool {
	var event irHistoryEvent
	if !decodeIR(raw, &event) || event.Scope == "" || len(event.Scope) > 128 {
		return false
	}
	switch event.Phase {
	case "message":
		return event.Index >= 0 && event.Count == 0 && validIRHistoryMessage(event.Message)
	case "commit":
		return event.Count >= 0 && event.Index == 0 && event.Message == nil
	default:
		return false
	}
}

func sessionIRHistorySave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	// session, checkpoint, scope, authenticated actor, canonical history
	if f[1] == "" || len(f[1]) > 128 || f[2] == "" || len(f[2]) > 128 {
		return store.StatusInvalid, nil, nil
	}
	var messages []json.RawMessage
	if !decodeIR([]byte(f[4]), &messages) || messages == nil {
		return store.StatusInvalid, nil, nil
	}
	// Validate the entire replacement before appending any part of it.
	events := make([]string, 0, len(messages)+1)
	for i, message := range messages {
		if !validIRHistoryMessage(message) {
			return store.StatusInvalid, nil, nil
		}
		payload, err := encodeSessionIR(struct {
			Kind    string         `json:"kind"`
			Payload irHistoryEvent `json:"payload"`
		}{"history", irHistoryEvent{Scope: f[2], Phase: "message", Index: i, Message: message}})
		if err != nil {
			return 0, nil, err
		}
		if !validSessionIREvent(payload) {
			return store.StatusInvalid, nil, nil
		}
		events = append(events, string(payload))
	}
	commit, err := encodeSessionIR(struct {
		Kind    string         `json:"kind"`
		Payload irHistoryEvent `json:"payload"`
	}{"history", irHistoryEvent{Scope: f[2], Phase: "commit", Count: len(messages)}})
	if err != nil {
		return 0, nil, err
	}
	events = append(events, string(commit))
	for i, event := range events {
		status, _, err := sessionIRAppend(ctx, q, []string{f[0], f[1] + ":" + strconv.Itoa(i), f[1], f[3], event})
		if err != nil || status != store.StatusOK {
			return status, nil, err
		}
	}
	return store.StatusOK, nil, nil
}

const sessionIRHistorySQL = `SELECT turn_id, payload FROM session_ir_events
 WHERE session_id=$1 AND EXISTS(SELECT 1 FROM server_sessions WHERE id=$1 AND principal=$2)
 ORDER BY sequence`

func sessionIRHistoryLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	rows, err := q.Query(ctx, sessionIRHistorySQL, f[0], f[2])
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	history := make([]json.RawMessage, 0)
	checkpoints := map[string][]json.RawMessage{}
	pending := map[string][]json.RawMessage{}
	positions := map[string]map[string]int{}
	found := false
	open := map[string]bool{}
	for rows.Next() {
		var turn, payload string
		if err = rows.Scan(&turn, &payload); err != nil {
			return 0, nil, err
		}
		var event struct {
			Kind      string          `json:"kind"`
			MessageID string          `json:"message_id"`
			Payload   json.RawMessage `json:"payload"`
		}
		if err = json.Unmarshal([]byte(payload), &event); err != nil {
			return 0, nil, err
		}
		switch event.Kind {
		case "history":
			var h irHistoryEvent
			if !decodeIR(event.Payload, &h) || !validIRHistoryEvent(event.Payload) {
				return 0, nil, fmt.Errorf("invalid canonical history checkpoint")
			}
			if h.Scope != f[1] {
				continue
			}
			if h.Phase == "message" {
				if h.Index != len(checkpoints[turn]) {
					return 0, nil, fmt.Errorf("unordered canonical history checkpoint")
				}
				checkpoints[turn] = append(checkpoints[turn], h.Message)
			} else {
				if h.Count != len(checkpoints[turn]) {
					return 0, nil, fmt.Errorf("incomplete canonical history checkpoint")
				}
				history = append(history[:0], checkpoints[turn]...)
				checkpoints = map[string][]json.RawMessage{}
				// All prior observations are incorporated by this host projection. A later
				// execution-stop must not append the same observed conversation again.
				pending = map[string][]json.RawMessage{}
				positions = map[string]map[string]int{}
				found = true
			}
		case "message":
			if f[1] != "primary" {
				continue
			}
			if !validIRHistoryMessage(event.Payload) {
				return 0, nil, fmt.Errorf("invalid canonical message")
			}
			if positions[turn] == nil {
				positions[turn] = map[string]int{}
			}
			if index, ok := positions[turn][event.MessageID]; ok {
				pending[turn][index] = event.Payload
			} else {
				positions[turn][event.MessageID] = len(pending[turn])
				pending[turn] = append(pending[turn], event.Payload)
			}
		case "execution":
			if f[1] != "primary" {
				continue
			}
			var execution struct {
				Status string `json:"status"`
			}
			if err = json.Unmarshal(event.Payload, &execution); err != nil {
				return 0, nil, err
			}
			if execution.Status == "running" {
				open[turn] = true
			}
			if execution.Status == "completed" || execution.Status == "failed" || execution.Status == "cancelled" {
				delete(open, turn)
				if len(pending[turn]) > 0 {
					history = append(history, pending[turn]...)
					found = true
				}
				delete(pending, turn)
				delete(positions, turn)
			}
		}
	}
	if err = rows.Err(); err != nil {
		return 0, nil, err
	}
	for turn := range open {
		if turn != f[3] {
			return 0, nil, fmt.Errorf("unfinished canonical execution requires reconciliation")
		}
	}
	if !found {
		return store.StatusMissing, nil, nil
	}
	encoded, err := encodeSessionIR(history)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{string(encoded)}, nil
}
