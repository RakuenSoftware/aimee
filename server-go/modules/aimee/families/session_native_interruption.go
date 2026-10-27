package families

import (
	"context"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"sort"
)

// Final snapshots replace earlier snapshots of the same message. Assistant
// TURN_START fixes first-observation order when a tool result arrives before
// the final assistant snapshot. No missing delta manufactures a call.
type interruptionObservation struct {
	Source canonical.ToolInterruptionSource
	Event  json.RawMessage
	Order  uint64
}

func nativeUnansweredCalls(observations []interruptionObservation) ([]canonical.ToolInterruptionSource, error) {
	final := map[string]interruptionObservation{}
	first := map[string]uint64{}
	protected := map[string]nativeOwnedMessage{}
	firstCalls := map[string]map[int]canonical.ToolInterruptionSource{}
	for _, o := range observations {
		var e struct {
			Kind      string                          `json:"kind"`
			MessageID string                          `json:"message_id"`
			Role      string                          `json:"role"`
			Payload   json.RawMessage                 `json:"payload"`
			Owner     *canonical.NativeExecutionOwner `json:"native_owner,omitempty"`
			Partial   json.RawMessage                 `json:"partial,omitempty"`
		}
		if !canonical.ValidSessionEvent(o.Event) || json.Unmarshal(o.Event, &e) != nil {
			return nil, fmt.Errorf("invalid native interruption source")
		}
		if e.Kind == "delta" && e.Role == "assistant" {
			var d struct {
				Type string `json:"type"`
			}
			if json.Unmarshal(e.Payload, &d) == nil && d.Type == "TURN_START" {
				if _, ok := first[e.MessageID]; !ok {
					first[e.MessageID] = o.Order
				}
			}
		}
		if e.Kind != "message" {
			continue
		}
		// ValidSessionEvent already validates every message block and payload field.
		if e.MessageID == "" {
			return nil, fmt.Errorf("invalid native interruption message")
		}
		var owned nativeOwnedMessage
		if json.Unmarshal(o.Event, &owned) != nil {
			return nil, fmt.Errorf("invalid message provenance")
		}
		if previous, exists := protected[e.MessageID]; exists {
			if !nativeSnapshotExtends(previous, owned) {
				return nil, fmt.Errorf("protected native snapshot replaced")
			}
		}
		if owned.Owner != nil {
			protected[e.MessageID] = owned
			if firstCalls[e.MessageID] == nil {
				firstCalls[e.MessageID] = map[int]canonical.ToolInterruptionSource{}
			}
			for index, block := range owned.Payload.Blocks {
				if block.Type == "tool_use" {
					if _, exists := firstCalls[e.MessageID][index]; !exists {
						ref := o.Source
						ref.MessageID = e.MessageID
						ref.BlockIndex = index
						ref.ToolID = block.ToolID
						ref.Name = block.Name
						firstCalls[e.MessageID][index] = ref
					}
				}
			}
		}
		previous, seen := final[e.MessageID]
		if seen {
			o.Order = previous.Order
		} else if n, ok := first[e.MessageID]; ok {
			o.Order = n
		}
		o.Source.MessageID = e.MessageID
		final[e.MessageID] = o
	}
	ordered := make([]interruptionObservation, 0, len(final))
	for _, v := range final {
		ordered = append(ordered, v)
	}
	sort.Slice(ordered, func(i, j int) bool { return ordered[i].Order < ordered[j].Order })
	var pending, unanswered []canonical.ToolInterruptionSource
	for _, o := range ordered {
		var e struct {
			Payload canonical.Message `json:"payload"`
		}
		if json.Unmarshal(o.Event, &e) != nil {
			return nil, fmt.Errorf("invalid native interruption blocks")
		}
		hasResult := false
		for _, b := range e.Payload.Blocks {
			hasResult = hasResult || b.Type == "tool_result"
		}
		if !hasResult {
			unanswered = append(unanswered, pending...)
			pending = nil
		}
		for _, b := range e.Payload.Blocks {
			if b.Type != "tool_result" {
				continue
			}
			matched := -1
			for i, p := range pending {
				if p.ToolID == b.ToolID {
					matched = i
					break
				}
			}
			if matched < 0 {
				return nil, fmt.Errorf("orphan native tool result requires separate reconciliation")
			}
			pending = append(pending[:matched], pending[matched+1:]...)
		}
		for index, b := range e.Payload.Blocks {
			if b.Type != "tool_use" {
				continue
			}
			if e.Payload.Role != "assistant" {
				return nil, fmt.Errorf("non-assistant native call source")
			}
			for _, p := range pending {
				if p.ToolID == b.ToolID {
					return nil, fmt.Errorf("ambiguous native tool call identity")
				}
			}
			source := o.Source
			if first, ok := firstCalls[o.Source.MessageID][index]; ok {
				source = first
			}
			source.BlockIndex = index
			source.ToolID = b.ToolID
			source.Name = b.Name
			pending = append(pending, source)
		}
	}
	unanswered = append(unanswered, pending...)
	if len(unanswered) > 128 {
		return nil, fmt.Errorf("native interrupted calls exceed bounded receipt capacity")
	}
	return unanswered, nil
}

func nativeInterruptionSources(ctx context.Context, q store.Queryer, v nativeReservation) ([]canonical.ToolInterruptionSource, error) {
	var observed []interruptionObservation
	ownedSources := map[string]bool{}
	bytes := 0
	last := "0"
	for {
		// Eight <=1 MiB canonical events plus row metadata fit the SQL
		// transport frame. Page before transport, not after decoding its reply.
		rows, err := q.Query(ctx, `SELECT sequence::text,event_id,actor,payload FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND sequence>$3::bigint AND (payload::jsonb->>'kind'='message' OR (payload::jsonb->>'kind'='delta' AND payload::jsonb->'payload'->>'type'='TURN_START')) ORDER BY session_ir_events.sequence LIMIT 8`, v.Session, v.Turn, last)
		if err != nil {
			return nil, err
		}
		pageCount := 0
		err = func() error {
			defer rows.Close()
			for rows.Next() {
				var o interruptionObservation
				o.Source.Session = v.Session
				o.Source.Turn = v.Turn
				var raw string
				if err := rows.Scan(&o.Source.Sequence, &o.Source.EventID, &o.Source.Actor, &raw); err != nil {
					return err
				}
				last = o.Source.Sequence
				pageCount++
				bytes += len(raw)
				if bytes > 16<<20 {
					return fmt.Errorf("native interruption source byte capacity exceeded")
				}
				o.Event = json.RawMessage(raw)
				var owned nativeOwnedMessage
				if json.Unmarshal(o.Event, &owned) != nil {
					return fmt.Errorf("invalid native source envelope")
				}
				if owned.Kind == "message" && owned.Owner != nil {
					expected := canonical.NativeExecutionOwner{Version: 1, Harness: v.Harness, ToolInterruptionOwner: canonical.ToolInterruptionOwner{Owner: v.Owner, Binding: v.Binding, Epoch: v.Epoch, Containment: v.Containment}}
					ownedSources[o.Source.Sequence] = *owned.Owner == expected
				}
				if o.Source.Actor != v.Actor {
					return fmt.Errorf("foreign native tool source actor")
				}
				o.Order = uint64(len(observed) + 1)
				observed = append(observed, o)
				if len(observed) > 4096 {
					return fmt.Errorf("native interruption source capacity exceeded")
				}
			}
			return rows.Err()
		}()
		if err != nil {
			return nil, err
		}
		if pageCount < 8 {
			break
		}
	}
	calls, err := nativeUnansweredCalls(observed)
	if err != nil {
		return nil, err
	}
	ownedCalls := make([]canonical.ToolInterruptionSource, 0, len(calls))
	for _, call := range calls {
		if ownedSources[call.Sequence] {
			ownedCalls = append(ownedCalls, call)
		}
	}
	return ownedCalls, nil
}

func nativeRecordInterruptions(ctx context.Context, q store.Queryer, v nativeReservation, terminal string) error {
	sources, err := nativeInterruptionSources(ctx, q, v)
	if err != nil {
		return err
	}
	for _, source := range sources {
		receipt := canonical.ToolInterruption{Version: 1, Source: source, Owner: canonical.ToolInterruptionOwner{Owner: v.Owner, Binding: v.Binding, Epoch: v.Epoch, Containment: v.Containment}, Terminal: terminal, Outcome: "unknown", Retry: "explicit_permission_required"}
		raw, err := json.Marshal(struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ToolInterruption `json:"payload"`
		}{"tool_interruption", receipt})
		if err != nil {
			return err
		}
		id := fmt.Sprintf("native-interruption:%s:%s:%d", v.Owner, source.Sequence, source.BlockIndex)
		status, _, err := sessionIRAppendAuthorized(ctx, q, []string{v.Session, id, v.Turn, v.Actor, string(raw)}, false, true, false)
		if err != nil {
			return err
		}
		if status != store.StatusOK {
			return fmt.Errorf("native interruption receipt append refused")
		}
	}
	return nil
}

type nativeHistorySource struct {
	Index  int
	Source canonical.ToolInterruptionSource
	Owner  *canonical.NativeExecutionOwner
	Blocks map[int]bool
	Calls  map[int]canonical.Block
}

func projectNativeInterruptions(history []json.RawMessage, sources map[string]nativeHistorySource, terminals map[string]string, receipts []struct {
	ID      string
	Receipt canonical.ToolInterruption
}, operatorTerminals ...map[string]string) ([]json.RawMessage, error) {
	grouped := map[int][]json.RawMessage{}
	seen := map[string]bool{}
	for _, entry := range receipts {
		r := entry.Receipt
		s := r.Source
		source, ok := sources[s.Sequence]
		ref := source.Source
		ownerValid := source.Owner != nil && source.Owner.ToolInterruptionOwner == r.Owner
		if r.Owner.OperatorCancel != "" {
			ownerValid = source.Owner == nil && len(operatorTerminals) == 1 && operatorTerminals[0][s.Turn] == r.Terminal && r.Owner.OperatorCancel == operatorCancelPrefix+s.Turn
		}
		if !ok || ref.Session != s.Session || ref.Turn != s.Turn || ref.EventID != s.EventID || ref.Actor != s.Actor || ref.MessageID != s.MessageID || !ownerValid || !source.Blocks[s.BlockIndex] || terminals[s.Turn] != r.Terminal {
			return nil, fmt.Errorf("interrupted call immutable source missing")
		}
		if source.Index < 0 {
			// A checkpoint may retain a previously host-projected unknown
			// result. Its marker is only a reference: original owned source and
			// terminal above still supply authority, never checkpoint content.
			found := false
			var pending *canonical.Block
			for _, raw := range history {
				var m canonical.Message
				if !canonical.Decode(raw, &m) {
					return nil, fmt.Errorf("invalid checkpoint message")
				}
				for _, b := range m.Blocks {
					if b.ToolID == s.ToolID && b.Type == "tool_use" {
						copy := b
						pending = &copy
					}
					if b.Type == "tool_result" && b.Context != nil && b.Context.RevisionDomain == canonical.ToolInterruptionDomain && b.Context.RevisionScope == entry.ID {
						var warning string
						expected, exists := source.Calls[s.BlockIndex]
						if found || pending == nil || !exists || !sameNativeCall(*pending, expected) || b.ToolID != s.ToolID || b.IsError == nil || !*b.IsError || json.Unmarshal(b.Content, &warning) != nil || warning != canonical.ToolInterruptionText {
							return nil, fmt.Errorf("checkpoint interruption provenance mismatch")
						}
						found = true
					}
					if b.ToolID == s.ToolID && b.Type == "tool_result" {
						pending = nil
					}
				}
			}
			if r.Owner.OperatorCancel != "" && pending != nil && !found {
				return nil, fmt.Errorf("operator checkpoint retains an unresolved call without its unknown diagnostic")
			}
			continue // omitted observations remain durable permission fences.
		}
		var message canonical.Message
		if !canonical.Decode(history[source.Index], &message) || message.Role != "assistant" || s.BlockIndex >= len(message.Blocks) {
			return nil, fmt.Errorf("interrupted call source block missing")
		}
		block := message.Blocks[s.BlockIndex]
		key := fmt.Sprintf("%s:%d", s.Sequence, s.BlockIndex)
		if block.Type != "tool_use" || block.ToolID != s.ToolID || block.Name != s.Name || seen[key] {
			return nil, fmt.Errorf("interrupted call source block mismatch")
		}
		seen[key] = true
		projected, err := canonical.InterruptionMessageJSON(r, entry.ID)
		if err != nil {
			return nil, err
		}
		var messageWire struct {
			Role   string            `json:"role"`
			Blocks []json.RawMessage `json:"blocks"`
		}
		if !canonical.ValidMessage(projected) || !canonical.Decode(projected, &messageWire) {
			return nil, fmt.Errorf("invalid generated interruption message")
		}
		grouped[source.Index] = append(grouped[source.Index], messageWire.Blocks...)
	}
	out := make([]json.RawMessage, 0, len(history)+len(grouped))
	for i, message := range history {
		out = append(out, message)
		if blocks := grouped[i]; len(blocks) > 0 {
			raw, err := json.Marshal(struct {
				Role   string            `json:"role"`
				Blocks []json.RawMessage `json:"blocks"`
			}{"tool", blocks})
			if err != nil {
				return nil, err
			}
			out = append(out, raw)
		}
	}
	return out, nil
}

const opSessionNativeHistory uint32 = 45

// Only a host consumer may receive the admission sidecar. Ordinary history
// remains a display/context projection and does not grant tool execution.
func sessionNativeHistory(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 4 || f[0] == "" || f[1] == "" || (f[2] != "completed" && f[2] != "running") || len(f[3]) > 128 {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], f[1])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	var cells []string
	if f[2] == "running" {
		status, cells, err = sessionIRRunningHistoryLoad(ctx, q, []string{f[1], f[0], f[3]})
	} else {
		status, cells, err = sessionIRHistoryLoad(ctx, q, []string{f[1], "primary", f[0], f[3]})
	}
	if err != nil {
		return 0, nil, err
	}
	if status == store.StatusMissing && f[2] == "completed" {
		cells = []string{"[]"}
		status = store.StatusOK
	}
	if status != store.StatusOK || len(cells) != 1 {
		return status, nil, nil
	}
	var messages []canonical.Message
	if !canonical.Decode([]byte(cells[0]), &messages) {
		return 0, nil, fmt.Errorf("invalid native admission history")
	}
	wanted := map[string]bool{}
	for _, m := range messages {
		for _, b := range m.Blocks {
			if b.Context != nil && b.Context.RevisionDomain == canonical.ToolInterruptionDomain {
				wanted[b.Context.RevisionScope] = true
			}
		}
	}
	rows, err := q.Query(ctx, `SELECT event_id FROM session_ir_events WHERE session_id=$1 AND actor=$2 AND payload::jsonb->>'kind'='tool_interruption' ORDER BY session_ir_events.sequence LIMIT 4097`, f[1], f[0])
	if err != nil {
		return 0, nil, err
	}
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return 0, nil, err
		}
		wanted[id] = true
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, nil, err
	}
	type entry struct {
		ID      string                     `json:"event_id"`
		Receipt canonical.ToolInterruption `json:"receipt"`
		Call    json.RawMessage            `json:"call"`
	}
	if len(wanted) > 4096 {
		return 0, nil, fmt.Errorf("native interruption sidecar capacity exceeded")
	}
	receipts := make([]entry, 0, len(wanted))
	operatorCalls := map[string][]canonical.ToolInterruptionSource{}
	for id := range wanted {
		var raw, actor string
		err := q.QueryRow(ctx, `SELECT payload,actor FROM session_ir_events WHERE session_id=$1 AND event_id=$2`, f[1], id).Scan(&raw, &actor)
		if err != nil {
			return 0, nil, err
		}
		var e struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ToolInterruption `json:"payload"`
		}
		if actor != f[0] || !canonical.ValidSessionEvent([]byte(raw)) || !canonical.Decode([]byte(raw), &e) || e.Kind != "tool_interruption" || e.Payload.Source.Session != f[1] || e.Payload.Source.Actor != f[0] {
			return 0, nil, fmt.Errorf("invalid native interruption authority")
		}
		var original string
		if err := q.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id=$1 AND sequence=$2::bigint AND event_id=$3 AND actor=$4 AND turn_id=$5`, f[1], e.Payload.Source.Sequence, e.Payload.Source.EventID, f[0], e.Payload.Source.Turn).Scan(&original); err != nil {
			return 0, nil, err
		}
		var sourceMessage struct {
			Kind      string                          `json:"kind"`
			MessageID string                          `json:"message_id"`
			Role      string                          `json:"role,omitempty"`
			Payload   json.RawMessage                 `json:"payload"`
			Owner     *canonical.NativeExecutionOwner `json:"native_owner,omitempty"`
			Partial   json.RawMessage                 `json:"partial,omitempty"`
		}
		var body canonical.Message
		if !canonical.Decode([]byte(original), &sourceMessage) || sourceMessage.Kind != "message" || sourceMessage.MessageID != e.Payload.Source.MessageID || !canonical.Decode(sourceMessage.Payload, &body) || body.Role != "assistant" || e.Payload.Source.BlockIndex >= len(body.Blocks) {
			return 0, nil, fmt.Errorf("native interruption call source disappeared")
		}
		operator := e.Payload.Owner.OperatorCancel != ""
		if operator {
			if sourceMessage.Owner != nil || id != fmt.Sprintf("operator-interruption:%s:%s:%d", e.Payload.Source.Turn, e.Payload.Source.Sequence, e.Payload.Source.BlockIndex) {
				return 0, nil, fmt.Errorf("operator interruption source is not an API call")
			}
			if err := operatorReceiptAuthority(ctx, q, e.Payload); err != nil {
				return 0, nil, err
			}
			calls, seen := operatorCalls[e.Payload.Source.Turn]
			if !seen {
				var err error
				calls, err = operatorInterruptionSources(ctx, q, f[1], e.Payload.Source.Turn, f[0])
				if err != nil {
					return 0, nil, err
				}
				operatorCalls[e.Payload.Source.Turn] = calls
			}
			found := false
			for _, call := range calls {
				found = found || call == e.Payload.Source
			}
			if !found {
				return 0, nil, fmt.Errorf("operator interruption no longer identifies an unanswered call")
			}
		} else if sourceMessage.Owner == nil || sourceMessage.Owner.ToolInterruptionOwner != e.Payload.Owner {
			return 0, nil, fmt.Errorf("native interruption source owner disappeared")
		}
		block := body.Blocks[e.Payload.Source.BlockIndex]
		if block.Type != "tool_use" || block.ToolID != e.Payload.Source.ToolID || block.Name != e.Payload.Source.Name {
			return 0, nil, fmt.Errorf("interruption call identity changed")
		}
		var sourceBlocks struct {
			Blocks []json.RawMessage `json:"blocks"`
		}
		if json.Unmarshal(sourceMessage.Payload, &sourceBlocks) != nil {
			return 0, nil, fmt.Errorf("native interruption source blocks unavailable")
		}
		receipts = append(receipts, entry{id, e.Payload, sourceBlocks.Blocks[e.Payload.Source.BlockIndex]})
	}
	sort.Slice(receipts, func(i, j int) bool { return receipts[i].ID < receipts[j].ID })
	raw, err := json.Marshal(struct {
		Messages      json.RawMessage `json:"messages"`
		Interruptions []entry         `json:"interruptions"`
	}{json.RawMessage(cells[0]), receipts})
	if len(raw) > 16<<20 {
		return 0, nil, fmt.Errorf("native history sidecar exceeds bounded response")
	}
	return store.StatusOK, []string{string(raw)}, err
}
