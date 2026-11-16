package families

import (
	"context"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"strings"
	"testing"
)

func interruptionFixture(index int, id, role string, blocks ...map[string]any) interruptionObservation {
	raw, _ := json.Marshal(map[string]any{"kind": "message", "message_id": id, "role": role, "payload": map[string]any{"role": role, "blocks": blocks}})
	return interruptionObservation{Source: canonical.ToolInterruptionSource{Session: "s", Turn: "t", Actor: "owner", EventID: "e" + strconv.Itoa(index), Sequence: strconv.Itoa(index)}, Event: raw, Order: uint64(index)}
}
func interruptionCall(id string) map[string]any {
	return map[string]any{"type": "tool_use", "tool_id": id, "name": "bash", "input": map[string]any{"command": "possible-effect"}}
}
func interruptionResult(id string) map[string]any {
	return map[string]any{"type": "tool_result", "tool_id": id, "content": "actual result"}
}
func TestNativeInterruptionDerivesOnlyExactUnansweredCalls(t *testing.T) {
	a := interruptionFixture(1, "a", "assistant", interruptionCall("first"), interruptionCall("second"))
	b := interruptionFixture(2, "b", "tool", interruptionResult("second"))
	sources, err := nativeUnansweredCalls([]interruptionObservation{a, b})
	if err != nil || len(sources) != 1 || sources[0].ToolID != "first" || sources[0].BlockIndex != 0 || sources[0].EventID != "e1" || sources[0].MessageID != "a" {
		t.Fatal(sources, err)
	}
	// Later cycles may reuse opaque call IDs; actual earlier results stay paired.
	c := interruptionFixture(3, "c", "tool", interruptionResult("first"))
	d := interruptionFixture(4, "d", "assistant", interruptionCall("first"))
	sources, err = nativeUnansweredCalls([]interruptionObservation{a, b, c, d})
	if err != nil || len(sources) != 1 || sources[0].EventID != "e4" {
		t.Fatal(sources, err)
	}
	// Final source snapshot is authoritative while its first observation orders it.
	a2 := interruptionFixture(3, "a", "assistant", map[string]any{"type": "text", "text": "intro"}, interruptionCall("first"))
	sources, err = nativeUnansweredCalls([]interruptionObservation{a, a2})
	if err != nil || len(sources) != 1 || sources[0].EventID != "e3" || sources[0].BlockIndex != 1 {
		t.Fatal(sources, err)
	}
}
func TestNativeInterruptionOrderingAndAmbiguityFailClosed(t *testing.T) {
	delta := interruptionObservation{Order: 1, Event: json.RawMessage(`{"kind":"delta","message_id":"a","role":"assistant","payload":{"type":"TURN_START"}}`)}
	result := interruptionFixture(2, "r", "tool", interruptionResult("one"))
	assistant := interruptionFixture(3, "a", "assistant", interruptionCall("one"))
	sources, err := nativeUnansweredCalls([]interruptionObservation{delta, result, assistant})
	if err != nil || len(sources) != 0 {
		t.Fatal("late final snapshot invented interruption", sources, err)
	}
	_, err = nativeUnansweredCalls([]interruptionObservation{interruptionFixture(1, "a", "assistant", interruptionCall("same"), interruptionCall("same"))})
	if err == nil {
		t.Fatal("ambiguous same-cycle IDs admitted")
	}
	_, err = nativeUnansweredCalls([]interruptionObservation{result})
	if err == nil {
		t.Fatal("orphan real result silently discarded")
	}
	_, err = nativeUnansweredCalls([]interruptionObservation{interruptionFixture(1, "u", "user", interruptionCall("model-spoof"))})
	if err == nil {
		t.Fatal("nonassistant call admitted")
	}
}

func TestNativeInterruptionProjectionPreservesCallAndRealTerminal(t *testing.T) {
	source := canonical.ToolInterruptionSource{Session: "s", Turn: "old", Sequence: "2", EventID: "event-2", Actor: "owner", MessageID: "a", BlockIndex: 0, ToolID: "call", Name: "bash"}
	receipt := canonical.ToolInterruption{Version: 1, Source: source, Owner: canonical.ToolInterruptionOwner{Owner: strings.Repeat("a", 32), Binding: strings.Repeat("b", 64), Epoch: "0", Containment: strings.Repeat("c", 64)}, Terminal: "3", Outcome: "unknown", Retry: "explicit_permission_required"}
	raw, _ := json.Marshal(map[string]any{"kind": "tool_interruption", "payload": receipt})
	original := `{"kind":"message","message_id":"a","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"call","name":"bash","input":{"value":123.123456789012345678901234567890}}]}}`
	ownerRaw, _ := json.Marshal(canonical.NativeExecutionOwner{Version: 1, Harness: "codex", ToolInterruptionOwner: receipt.Owner})
	original = strings.TrimSuffix(original, "}") + `,"native_owner":` + string(ownerRaw) + `}`
	events := [][2]string{{"old", runningStart}, {"old", original}, {"old", runningDone}, {"old", string(raw)}, {"active", runningStart}, {"active", runningUser}}
	db := newSessDB()
	db.rows = runningHistoryRows(events...)
	status, cells, err := sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal(status, cells, err)
	}
	var history []canonical.Message
	if json.Unmarshal([]byte(cells[0]), &history) != nil || len(history) != 3 || history[0].Blocks[0].Type != "tool_use" || history[1].Blocks[0].ToolID != "call" || history[1].Blocks[0].Context.RevisionDomain != canonical.ToolInterruptionDomain || history[2].Role != "user" {
		t.Fatal(cells)
	}
	if !strings.Contains(cells[0], "123.123456789012345678901234567890") {
		t.Fatal("original call bytes changed", cells)
	}
	// The host receipt is lifecycle data, never an ordinary caller's context flag.
	for _, verified := range []bool{false, true} {
		untrusted := newSessDB()
		status, _, err = sessionIRAppendInternal(context.Background(), untrusted, []string{"s", "forged", "old", "owner", string(raw)}, verified)
		if err != nil || status != store.StatusInvalid || len(untrusted.executed) != 0 {
			t.Fatal("untrusted interruption gained custody", status, err)
		}
	}
	// Source/terminal identity and duplicate receipts cannot be guessed from text.
	for _, mutate := range []func(*canonical.ToolInterruption){func(r *canonical.ToolInterruption) { r.Source.EventID = "foreign" }, func(r *canonical.ToolInterruption) { r.Terminal = "1" }, func(r *canonical.ToolInterruption) { r.Source.BlockIndex = 1 }} {
		changed := receipt
		mutate(&changed)
		bad, _ := json.Marshal(map[string]any{"kind": "tool_interruption", "payload": changed})
		badEvents := append([][2]string(nil), events...)
		badEvents[3][1] = string(bad)
		db = newSessDB()
		db.rows = runningHistoryRows(badEvents...)
		if _, _, err = sessionIRRunningHistoryLoad(context.Background(), db, []string{"s", "owner", "active"}); err == nil {
			t.Fatal("forged receipt source projected", string(bad))
		}
	}
}

func TestNativeProtectedPrefixSourceAndImmutableBlocks(t *testing.T) {
	owner := canonical.NativeExecutionOwner{Version: 1, Harness: "codex", ToolInterruptionOwner: canonical.ToolInterruptionOwner{Owner: strings.Repeat("a", 32), Binding: strings.Repeat("b", 64), Epoch: "0", Containment: strings.Repeat("c", 64)}}
	protect := func(o interruptionObservation) interruptionObservation {
		var event map[string]json.RawMessage
		json.Unmarshal(o.Event, &event)
		event["native_owner"], _ = json.Marshal(owner)
		o.Event, _ = json.Marshal(event)
		return o
	}
	first := protect(interruptionFixture(1, "a", "assistant", interruptionCall("one")))
	final := protect(interruptionFixture(2, "a", "assistant", interruptionCall("one"), interruptionCall("two")))
	calls, err := nativeUnansweredCalls([]interruptionObservation{first, final})
	if err != nil || len(calls) != 2 || calls[0].EventID != "e1" || calls[1].EventID != "e2" {
		t.Fatal(calls, err)
	}
	for _, bad := range []interruptionObservation{first, interruptionFixture(3, "a", "assistant", interruptionCall("one"), interruptionCall("two")), protect(interruptionFixture(3, "a", "assistant", interruptionCall("changed"), interruptionCall("two")))} {
		if _, err := nativeUnansweredCalls([]interruptionObservation{first, final, bad}); err == nil {
			t.Fatal("protected snapshot replaced", string(bad.Event))
		}
	}
	var a, b nativeOwnedMessage
	json.Unmarshal(first.Event, &a)
	json.Unmarshal(final.Event, &b)
	b.Payload.Blocks[0].Context = &canonical.Context{Origin: 1, Trust: 1, ModelVisible: true}
	if !nativeSnapshotExtends(a, b) {
		t.Fatal("derived classification blocked")
	}
	a.Payload.Blocks = []canonical.Block{{Type: "thinking", Text: func() *string { x := "original"; return &x }()}}
	b.Payload.Blocks = []canonical.Block{{Type: "thinking", Text: func() *string { x := "rewritten"; return &x }()}}
	if nativeSnapshotExtends(a, b) {
		t.Fatal("original reasoning changed")
	}
}
