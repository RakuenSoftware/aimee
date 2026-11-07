package ir

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestToolInterruptionIsStrictUnknownLifecycleMetadata(t *testing.T) {
	r := ToolInterruption{Version: 1, Source: ToolInterruptionSource{Session: "s", Turn: "t", Sequence: "2", EventID: "original", Actor: "owner", MessageID: "assistant", BlockIndex: 0, ToolID: "call", Name: "bash"}, Owner: ToolInterruptionOwner{Owner: strings.Repeat("a", 32), Binding: strings.Repeat("b", 64), Epoch: "0", Containment: strings.Repeat("c", 64)}, Terminal: "3", Outcome: "unknown", Retry: "explicit_permission_required"}
	raw, _ := json.Marshal(r)
	if !ValidToolInterruption(raw) {
		t.Fatal(string(raw))
	}
	var fields map[string]any
	json.Unmarshal(raw, &fields)
	for _, test := range []func(map[string]any){
		func(m map[string]any) { m["outcome"] = "cancelled" },
		func(m map[string]any) { m["outcome"] = "succeeded" },
		func(m map[string]any) { m["retry"] = "automatic" },
		func(m map[string]any) { m["source"].(map[string]any)["block_index"] = nil },
		func(m map[string]any) { delete(m["source"].(map[string]any), "block_index") },
		func(m map[string]any) { m["source"].(map[string]any)["sequence"] = "02" },
		func(m map[string]any) { m["owner"].(map[string]any)["epoch"] = "wrong" },
		func(m map[string]any) { m["trusted"] = true },
	} {
		json.Unmarshal(raw, &fields)
		test(fields)
		bad, _ := json.Marshal(fields)
		if ValidToolInterruption(bad) {
			t.Fatal("invalid lifecycle admitted", string(bad))
		}
	}
	message := InterruptionMessage(r, "receipt")
	encoded, _ := json.Marshal(message)
	if !ValidMessage(encoded) || message.Blocks[0].IsError == nil || !*message.Blocks[0].IsError || message.Blocks[0].Context.RevisionDomain != ToolInterruptionDomain {
		t.Fatal(string(encoded))
	}
	if strings.Contains(string(encoded), `"type":"tool_use"`) {
		t.Fatal("receipt reissues an action")
	}
}
