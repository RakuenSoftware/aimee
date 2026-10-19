package families

import (
	"context"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"sort"
	"strconv"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Checkpoints are host projections of admitted/executed IR, not model output.
// Observation events remain immutable, including calls removed by governance.
// The checkpoint and every message are appended in one owner-checked transaction.
func validIRHistoryMessage(raw json.RawMessage) bool { return canonical.ValidMessage(raw) }

type irHistoryEvent = canonical.HistoryEvent

func validIRHistoryEvent(raw json.RawMessage) bool { return canonical.ValidHistoryEvent(raw) }

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

const sessionIRRunningHistorySQL = `SELECT sequence::text, turn_id, actor, payload FROM session_ir_events
 WHERE session_id=$1 AND EXISTS(SELECT 1 FROM server_sessions WHERE id=$1 AND principal=$2)
 ORDER BY session_ir_events.sequence`

func sessionIRHistoryLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	return sessionIRHistoryProject(ctx, q, f, false)
}

// The host owns this running execution. Include its committed observations for
// native request reconciliation, without changing completed-history loading.
func sessionIRRunningHistoryLoad(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	return sessionIRHistoryProject(ctx, q, []string{f[0], "primary", f[1], f[2]}, true)
}

// A tool result can commit while the provider stream is still open. Retain the
// assistant message's first canonical observation order rather than the later
// full-snapshot arrival order. Unmatched deltas never manufacture messages.
func orderedIRPending(messages []json.RawMessage, order []uint64) []json.RawMessage {
	indices := make([]int, len(messages))
	for i := range indices {
		indices[i] = i
	}
	sort.SliceStable(indices, func(i, j int) bool { return order[indices[i]] < order[indices[j]] })
	result := make([]json.RawMessage, len(messages))
	for i, index := range indices {
		result[i] = messages[index]
	}
	return result
}

func sessionIRHistoryProject(ctx context.Context, q store.Queryer, f []string, running bool) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	query := sessionIRHistorySQL
	if running {
		query = sessionIRRunningHistorySQL
	}
	rows, err := q.Query(ctx, query, f[0], f[2])
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()
	history := make([]json.RawMessage, 0)
	checkpoints := map[string][]json.RawMessage{}
	pending := map[string][]json.RawMessage{}
	positions := map[string]map[string]int{}
	pendingOrder := map[string][]uint64{}
	firstObserved := map[string]map[string]uint64{}
	var rowOrder uint64
	found := false
	open := map[string]bool{}
	var previous uint64
	for rows.Next() {
		rowOrder++
		var turn, payload string
		if running {
			var sequence, actor string
			err = rows.Scan(&sequence, &turn, &actor, &payload)
			if err == nil {
				n, parseErr := strconv.ParseUint(sequence, 10, 64)
				if parseErr != nil || n == 0 || strconv.FormatUint(n, 10) != sequence || n != previous+1 {
					return 0, nil, fmt.Errorf("invalid running canonical history sequence %q after %d", sequence, previous)
				}
				if actor != f[2] {
					return 0, nil, fmt.Errorf("invalid running canonical history actor")
				}
				if !validSessionIREvent([]byte(payload)) {
					return 0, nil, fmt.Errorf("invalid running canonical history event at sequence %s", sequence)
				}
				previous = n
			}
		} else {
			err = rows.Scan(&turn, &payload)
		}
		if err != nil {
			return 0, nil, err
		}
		var event struct {
			Kind      string          `json:"kind"`
			MessageID string          `json:"message_id"`
			Role      string          `json:"role"`
			Payload   json.RawMessage `json:"payload"`
		}
		if err = json.Unmarshal([]byte(payload), &event); err != nil {
			return 0, nil, err
		}
		switch event.Kind {
		case "replay_proof", "native_checkpoint", "native_compaction":
			// Host-native provenance advances the source log, never model history.
			continue
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
				pendingOrder = map[string][]uint64{}
				firstObserved = map[string]map[string]uint64{}
				found = true
			}
		case "delta":
			if f[1] != "primary" || event.Role != "assistant" || event.MessageID == "" ||
				(!running && !validSessionIREvent([]byte(payload))) {
				continue
			}
			var delta struct {
				Type string `json:"type"`
			}
			if json.Unmarshal(event.Payload, &delta) != nil || delta.Type != "TURN_START" {
				continue
			}
			if firstObserved[turn] == nil {
				firstObserved[turn] = map[string]uint64{}
			}
			if _, seen := firstObserved[turn][event.MessageID]; !seen {
				if len(firstObserved[turn]) >= 4096 {
					return 0, nil, fmt.Errorf("canonical observation order exceeds capacity")
				}
				firstObserved[turn][event.MessageID] = rowOrder
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
				order := rowOrder
				var message struct {
					Role string `json:"role"`
				}
				if err = json.Unmarshal(event.Payload, &message); err != nil {
					return 0, nil, err
				}
				if first := firstObserved[turn][event.MessageID]; message.Role == "assistant" && first != 0 {
					order = first
				}
				pendingOrder[turn] = append(pendingOrder[turn], order)
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
					history = append(history, orderedIRPending(pending[turn], pendingOrder[turn])...)
					found = true
				}
				delete(pending, turn)
				delete(positions, turn)
				delete(pendingOrder, turn)
				delete(firstObserved, turn)
			}
		}
	}
	if err = rows.Err(); err != nil {
		return 0, nil, err
	}
	if running && len(checkpoints) != 0 {
		return 0, nil, fmt.Errorf("incomplete canonical history checkpoint")
	}
	for turn := range open {
		if turn != f[3] {
			return 0, nil, fmt.Errorf("unfinished canonical execution requires reconciliation")
		}
	}
	if running {
		if !open[f[3]] {
			return store.StatusMissing, nil, nil
		}
		for turn, messages := range pending {
			if turn != f[3] && len(messages) != 0 {
				return 0, nil, fmt.Errorf("unfinished canonical observations require reconciliation")
			}
		}
		history = append(history, orderedIRPending(pending[f[3]], pendingOrder[f[3]])...)
		found = true
	}
	if !found {
		return store.StatusMissing, nil, nil
	}
	encoded, err := encodeSessionIR(history)
	if err != nil {
		return 0, nil, err
	}
	if running {
		// Session observations allow diagnostic roles. A native request snapshot
		// must also pass the existing model-history admission contract. Retain
		// the original JSON, including exact tool arguments, after validation.
		if _, err = canonical.DecodeMessages(encoded); err != nil {
			return 0, nil, err
		}
	}
	return store.StatusOK, []string{string(encoded)}, nil
}
