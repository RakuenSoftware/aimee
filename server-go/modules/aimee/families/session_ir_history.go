package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"

	canonical "github.com/JBailes/aimee/server-go/ir"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

var errSessionIRUnfinishedExecution = errors.New("unfinished canonical execution requires reconciliation")

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

// Every page holds the same owner row lock until the enclosing transaction ends.
// Bound both row count and encoded text bytes below the SQL store reply limits.
// The reducer still checks every immutable source sequence without truncation.
const sessionIRHistoryPageSize = 256
const sessionIRHistorySQL = `WITH owned AS MATERIALIZED (
 SELECT id FROM server_sessions WHERE id=$1 AND principal=$2 FOR SHARE
), candidates AS MATERIALIZED (
 SELECT sequence,turn_id,actor,event_id,payload FROM session_ir_events
 WHERE session_id IN (SELECT id FROM owned) AND sequence>$3::bigint
 ORDER BY session_ir_events.sequence LIMIT 256
), budgeted AS (
 SELECT *,sum(octet_length(turn_id)+octet_length(actor)+octet_length(event_id)+octet_length(payload)+64)
 OVER (ORDER BY candidates.sequence) AS wire_bytes FROM candidates
) SELECT sequence::text,turn_id,actor,event_id,payload FROM budgeted
 WHERE wire_bytes<=8388608 ORDER BY budgeted.sequence`
const sessionIRRunningHistorySQL = sessionIRHistorySQL

type sessionIRHistoryRows struct {
	ctx                   context.Context
	q                     store.Queryer
	session, actor, after string
	rows                  store.Rows
	count                 int
	err                   error
	closed                bool
}

func (r *sessionIRHistoryRows) Next() bool {
	if r.closed || r.err != nil {
		return false
	}
	if r.rows.Next() {
		r.count++
		return true
	}
	r.err = r.rows.Err()
	r.rows.Close()
	if r.err != nil || r.count == 0 {
		r.closed = true
		return false
	}
	r.rows, r.err = r.q.Query(r.ctx, sessionIRHistorySQL, r.session, r.actor, r.after)
	r.count = 0
	if r.err != nil {
		return false
	}
	return r.Next()
}
func (r *sessionIRHistoryRows) Scan(dest ...any) error {
	if err := r.rows.Scan(dest...); err != nil {
		return err
	}
	sequence, ok := dest[0].(*string)
	if !ok {
		return fmt.Errorf("invalid canonical page cursor")
	}
	n, e := strconv.ParseUint(*sequence, 10, 64)
	previous, _ := strconv.ParseUint(r.after, 10, 64)
	if e != nil || n <= previous {
		return fmt.Errorf("canonical history page did not advance")
	}
	r.after = *sequence
	return nil
}
func (r *sessionIRHistoryRows) Err() error { return r.err }
func (r *sessionIRHistoryRows) Close() {
	if r.rows != nil {
		r.rows.Close()
	}
	r.closed = true
}

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

func sessionIRHistoryProject(ctx context.Context, q store.Queryer, f []string, running bool) (uint32, []string, error) {
	if f[0] == "" || f[1] == "" || f[2] == "" {
		return store.StatusInvalid, nil, nil
	}
	query := sessionIRHistorySQL
	if running {
		query = sessionIRRunningHistorySQL
	}
	first, err := q.Query(ctx, query, f[0], f[2], "0")
	if err != nil {
		return 0, nil, err
	}
	rows := &sessionIRHistoryRows{ctx: ctx, q: q, session: f[0], actor: f[2], after: "0", rows: first}
	defer rows.Close()
	history := make([]json.RawMessage, 0)
	checkpoints := map[string][]json.RawMessage{}
	imports := map[string]bool{}
	type fragmentState struct {
		index, next, count int
		hash               string
		text               strings.Builder
	}
	fragments := map[string]*fragmentState{}
	fragmentBytes := 0
	pending := map[string][]json.RawMessage{}
	positions := map[string]map[string]int{}
	pendingOrder := map[string][]uint64{}
	firstObserved := map[string]map[string]uint64{}
	var rowOrder uint64
	pendingRefs := map[string][]canonical.ToolInterruptionSource{}
	historyRefs := map[string]nativeHistorySource{}
	protected := map[string]map[string]nativeOwnedMessage{}
	protectedRefs := map[string]map[string]map[string]nativeHistorySource{}
	firstCalls := map[string]map[string]map[int]bool{}
	var interruptions []struct {
		ID      string
		Receipt canonical.ToolInterruption
	}
	terminals := map[string]string{}
	operatorTerminals := map[string]string{}
	apiLatest := map[string]map[string]string{}
	appendPending := func(turn string) {
		indices := make([]int, len(pending[turn]))
		for i := range indices {
			indices[i] = i
		}
		sort.SliceStable(indices, func(i, j int) bool { return pendingOrder[turn][indices[i]] < pendingOrder[turn][indices[j]] })
		for _, index := range indices {
			ref := pendingRefs[turn][index]
			mapped := historyRefs[ref.Sequence]
			mapped.Index = len(history)
			mapped.Source = ref
			historyRefs[ref.Sequence] = mapped
			for sequence, ownedRef := range protectedRefs[turn][ref.MessageID] {
				ownedRef.Index = len(history)
				historyRefs[sequence] = ownedRef
			}
			history = append(history, pending[turn][index])
		}
	}
	found := false
	open := map[string]bool{}
	var previous uint64
	for rows.Next() {
		rowOrder++
		var turn, payload, sequence, actor, eventID string
		err = rows.Scan(&sequence, &turn, &actor, &eventID, &payload)
		if err != nil {
			return 0, nil, err
		}
		n, parseErr := strconv.ParseUint(sequence, 10, 64)
		if parseErr != nil || n == 0 || strconv.FormatUint(n, 10) != sequence || n != previous+1 || actor != f[2] || eventID == "" || !validSessionIREvent([]byte(payload)) {
			return 0, nil, fmt.Errorf("invalid canonical history source at sequence %s", sequence)
		}
		previous = n
		var event struct {
			Kind      string                          `json:"kind"`
			MessageID string                          `json:"message_id"`
			Role      string                          `json:"role"`
			Payload   json.RawMessage                 `json:"payload"`
			Owner     *canonical.NativeExecutionOwner `json:"native_owner,omitempty"`
			Partial   json.RawMessage                 `json:"partial,omitempty"`
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
			if h.Phase == "import_report" || h.Phase == "archive" {
				if h.Phase == "import_report" && h.Scope == f[1] && (h.ImportReport.Outcome == "exact" || h.ImportReport.Outcome == "normalized") {
					imports[turn] = true
				}
				continue
			}
			if h.Scope != f[1] {
				continue
			}

			if h.Phase == "message_fragment" {
				part := h.Fragment
				current := fragments[turn]
				if part.PartIndex == 0 {
					if current != nil || part.MessageIndex != len(checkpoints[turn]) {
						return 0, nil, fmt.Errorf("unordered canonical message fragment")
					}
					current = &fragmentState{index: part.MessageIndex, count: part.PartCount, hash: part.MessageSHA256}
					fragments[turn] = current
				}
				if current == nil || part.MessageIndex != current.index || part.PartIndex != current.next || part.PartCount != current.count || part.MessageSHA256 != current.hash || fragmentBytes > 16*1024*1024-len(part.Text) {
					return 0, nil, fmt.Errorf("invalid canonical message fragment sequence")
				}
				current.text.WriteString(part.Text)
				fragmentBytes += len(part.Text)
				current.next++
				if current.next == current.count {
					raw := []byte(current.text.String())
					sum := sha256.Sum256(raw)
					if hex.EncodeToString(sum[:]) != current.hash || !validIRHistoryMessage(raw) {
						return 0, nil, fmt.Errorf("canonical message fragment digest or content mismatch")
					}
					checkpoints[turn] = append(checkpoints[turn], json.RawMessage(raw))
					delete(fragments, turn)
				}
				continue
			}
			if fragments[turn] != nil {
				return 0, nil, fmt.Errorf("incomplete canonical message fragment")
			}
			if h.Phase == "message" {
				if h.Index != len(checkpoints[turn]) {
					return 0, nil, fmt.Errorf("unordered canonical history checkpoint")
				}
				checkpoints[turn] = append(checkpoints[turn], h.Message)
			} else {
				if len(fragments) > 0 {
					return 0, nil, fmt.Errorf("checkpoint interrupted unfinished canonical message fragments")
				}
				if h.Count != len(checkpoints[turn]) {
					return 0, nil, fmt.Errorf("incomplete canonical history checkpoint")
				}
				history = append(history[:0], checkpoints[turn]...)
				checkpoints = map[string][]json.RawMessage{}
				fragmentBytes = 0
				// All prior observations are incorporated by this host projection. A later
				// execution-stop must not append the same observed conversation again.
				// An imported legacy base predates current captured user/tool
				// observations. Unlike a normal full checkpoint it contains none
				// of those pending turns, so their immutable source must survive.
				if !imports[turn] {
					pending = map[string][]json.RawMessage{}
					pendingRefs = map[string][]canonical.ToolInterruptionSource{}
					for sequence, ref := range historyRefs {
						ref.Index = -1
						historyRefs[sequence] = ref
					}
					protected = map[string]map[string]nativeOwnedMessage{}
					protectedRefs = map[string]map[string]map[string]nativeHistorySource{}
					firstCalls = map[string]map[string]map[int]bool{}
					positions = map[string]map[string]int{}
					pendingOrder = map[string][]uint64{}
					firstObserved = map[string]map[string]uint64{}
				}
				delete(imports, turn)
				found = true
			}
		case "tool_interruption":
			if f[1] != "primary" {
				continue
			}
			var receipt canonical.ToolInterruption
			if !canonical.Decode(event.Payload, &receipt) || receipt.Source.Session != f[0] || receipt.Source.Turn != turn || receipt.Source.Actor != actor {
				return 0, nil, fmt.Errorf("foreign interrupted call source")
			}
			interruptions = append(interruptions, struct {
				ID      string
				Receipt canonical.ToolInterruption
			}{eventID, receipt})
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
			ref := canonical.ToolInterruptionSource{Session: f[0], Turn: turn, Sequence: sequence, EventID: eventID, Actor: actor, MessageID: event.MessageID}
			var snapshot nativeOwnedMessage
			if json.Unmarshal([]byte(payload), &snapshot) != nil {
				return 0, nil, fmt.Errorf("invalid protected message")
			}
			if old, exists := protected[turn][event.MessageID]; exists && !nativeSnapshotExtends(old, snapshot) {
				return 0, nil, fmt.Errorf("protected native snapshot replaced")
			}
			if snapshot.Owner == nil {
				if apiLatest[turn] == nil {
					apiLatest[turn] = map[string]string{}
				}
				if prior := apiLatest[turn][event.MessageID]; prior != "" {
					old := historyRefs[prior]
					old.Blocks = map[int]bool{}
					historyRefs[prior] = old
				}
				apiLatest[turn][event.MessageID] = sequence
				blocks := map[int]bool{}
				calls := map[int]canonical.Block{}
				for i, b := range snapshot.Payload.Blocks {
					if b.Type == "tool_use" {
						blocks[i] = true
						calls[i] = b
					}
				}
				historyRefs[sequence] = nativeHistorySource{Index: -1, Source: ref, Blocks: blocks, Calls: calls}
			}
			if snapshot.Owner != nil {
				if protected[turn] == nil {
					protected[turn] = map[string]nativeOwnedMessage{}
					protectedRefs[turn] = map[string]map[string]nativeHistorySource{}
					firstCalls[turn] = map[string]map[int]bool{}
				}
				protected[turn][event.MessageID] = snapshot
				if protectedRefs[turn][event.MessageID] == nil {
					protectedRefs[turn][event.MessageID] = map[string]nativeHistorySource{}
					firstCalls[turn][event.MessageID] = map[int]bool{}
				}
				allowed := map[int]bool{}
				calls := map[int]canonical.Block{}
				for i, b := range snapshot.Payload.Blocks {
					if b.Type == "tool_use" && !firstCalls[turn][event.MessageID][i] {
						allowed[i] = true
						calls[i] = b
						firstCalls[turn][event.MessageID][i] = true
					}
				}
				protectedRefs[turn][event.MessageID][sequence] = nativeHistorySource{Source: ref, Owner: snapshot.Owner, Blocks: allowed, Calls: calls}
			}
			if index, ok := positions[turn][event.MessageID]; ok {
				pendingRefs[turn][index] = ref
				pending[turn][index] = event.Payload
			} else {
				positions[turn][event.MessageID] = len(pending[turn])
				pending[turn] = append(pending[turn], event.Payload)
				pendingRefs[turn] = append(pendingRefs[turn], ref)
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
				terminals[turn] = sequence
				if eventID == operatorCancelPrefix+turn && payload == operatorCancelPayload {
					operatorTerminals[turn] = sequence
				}
				delete(open, turn)
				if len(pending[turn]) > 0 {
					appendPending(turn)
					found = true
				}
				delete(pending, turn)
				delete(pendingRefs, turn)
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
			return 0, nil, errSessionIRUnfinishedExecution
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
		appendPending(f[3])
		found = true
	}
	if len(interruptions) > 0 {
		var err error
		history, err = projectNativeInterruptions(history, historyRefs, terminals, interruptions, operatorTerminals)
		if err != nil {
			return 0, nil, err
		}
	}
	if len(fragments) > 0 {
		return 0, nil, fmt.Errorf("unfinished canonical message fragments")
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
