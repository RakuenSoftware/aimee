package ir

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestOperatorInterruptionOwnerIsExclusiveAndActorBounded(t *testing.T) {
	r := ToolInterruption{Version: 1, Source: ToolInterruptionSource{Session: "s", Turn: "turn", Sequence: "9007199254740993", EventID: "call", Actor: strings.Repeat("a", 159), MessageID: "assistant", BlockIndex: 0, ToolID: "call-id", Name: "request_input"}, Owner: ToolInterruptionOwner{OperatorCancel: "operator-cancel:turn"}, Terminal: "9007199254740994", Outcome: "unknown", Retry: "explicit_permission_required"}
	raw, _ := json.Marshal(r)
	if !ValidToolInterruption(raw) {
		t.Fatal("valid operator interruption refused")
	}
	var fields map[string]any
	_ = json.Unmarshal(raw, &fields)
	if len(fields["owner"].(map[string]any)) != 1 {
		t.Fatal("native custody fields leaked into operator proof")
	}
	for _, mutate := range []func(*ToolInterruption){func(r *ToolInterruption) { r.Owner.Epoch = "0" }, func(r *ToolInterruption) { r.Owner.OperatorCancel = "operator-cancel:foreign" }, func(r *ToolInterruption) { r.Source.Actor += "a" }, func(r *ToolInterruption) { r.Outcome = "cancelled" }} {
		copy := r
		mutate(&copy)
		encoded, _ := json.Marshal(copy)
		if ValidToolInterruption(encoded) {
			t.Fatal("invalid mixed/operator shape accepted")
		}
	}
	native := NativeExecutionOwner{Version: 1, Harness: "codex", ToolInterruptionOwner: ToolInterruptionOwner{Owner: strings.Repeat("a", 32), Binding: strings.Repeat("b", 64), Epoch: "0", Containment: strings.Repeat("c", 64), OperatorCancel: "operator-cancel:turn"}}
	encoded, _ := json.Marshal(native)
	if ValidNativeExecutionOwner(encoded) {
		t.Fatal("operator cancellation became native custody")
	}
}
