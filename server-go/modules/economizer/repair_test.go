package economizer

import (
	"strings"
	"testing"
)

// These cases exercise the canonical cycle contract. Native protocol parsing
// and argument coercion are no longer responsibilities of the economizer.
func TestCanonicalRepairCycles(t *testing.T) {
	for _, test := range []struct {
		name, raw      string
		repairs, count int
	}{
		{"empty", `[]`, 0, 0},
		{"text", `[{"role":"user","blocks":[{"type":"text","text":"keep requirement"}]}]`, 0, 1},
		{"paired", `[{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"a","name":"Read","input":{}}]},{"role":"tool","blocks":[{"type":"tool_result","tool_id":"a","content":"ok"}]}]`, 0, 2},
		{"missing", `[{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"a","name":"Read","input":{}}]}]`, 1, 2},
		{"orphan", `[{"role":"tool","blocks":[{"type":"tool_result","tool_id":"unknown","content":"bad"}]}]`, 1, 0},
		{"mixed_result_text", `[{"role":"user","blocks":[{"type":"text","text":"keep protected text"},{"type":"tool_result","tool_id":"unknown","content":"bad"}]}]`, 1, 1},
		{"grouped_partial", `[{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"a","name":"Read","input":{}},{"type":"tool_use","tool_id":"b","name":"Read","input":{}}]},{"role":"user","blocks":[{"type":"tool_result","tool_id":"a","content":"ok"}]},{"role":"user","blocks":[{"type":"text","text":"next"}]}]`, 1, 4},
	} {
		t.Run(test.name, func(t *testing.T) {
			messages := ParseJSON(test.raw)
			if got := MessageHistoryRepair(messages); got != test.repairs || messages.Len() != test.count {
				t.Fatalf("repairs=%d count=%d history=%s", got, messages.Len(), PrintJSONUnformatted(messages))
			}
			if got := MessageHistoryRepair(messages); got != 0 {
				t.Fatalf("not idempotent: %d", got)
			}
			if test.name == "missing" && !strings.Contains(PrintJSONUnformatted(messages), cancelMsg) {
				t.Fatal("missing cancellation evidence")
			}
			if test.name == "mixed_result_text" && fixtureText(messages.At(0)) != "keep protected text" {
				t.Fatal("protected text lost")
			}
			if test.name == "grouped_partial" && messages.At(2).Get("blocks").At(0).GetString("tool_id") != "b" {
				t.Fatal("missing result not inserted at cycle boundary")
			}
		})
	}
}
func TestCanonicalRepairRefusalPreservesInput(t *testing.T) {
	for _, raw := range []string{
		`[{"role":"assistant","tool_calls":[]}]`,
		`[{"type":"function_call","call_id":"a","name":"Read","arguments":"{}"}]`,
		`[{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"","name":"Read","input":{}}]}]`,
		`[{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"dup","name":"Read","input":{}},{"type":"tool_use","tool_id":"dup","name":"Read","input":{}}]}]`,
		`[{"role":"user","blocks":[{"type":"text","text":"ok"}]},{"role":"user","content":"native bypass"}]`,
	} {
		messages := ParseJSON(raw)
		before := PrintJSONUnformatted(messages)
		if got := MessageHistoryRepair(messages); got != -1 {
			t.Fatalf("invalid history accepted: %d %s", got, raw)
		}
		if PrintJSONUnformatted(messages) != before {
			t.Fatal("refusal changed input")
		}
	}
}
func TestCanonicalRepairRetainsOpaqueBlocks(t *testing.T) {
	messages := ParseJSON(`[{"role":"assistant","blocks":[{"type":"thinking","text":"private","signature":"opaque-signature"},{"type":"tool_use","tool_id":"a","name":"Read","input":{"id":9007199254740993,"ratio":0.12345678901234567890123456789}}]}]`)
	before := PrintJSONUnformatted(messages.At(0))
	if got := MessageHistoryRepair(messages); got != 1 {
		t.Fatal(got)
	}
	if PrintJSONUnformatted(messages.At(0)) != before {
		t.Fatal("repair changed signed/opaque input")
	}
}

func TestCanonicalRepairReusedIDs(t *testing.T) {
	messages := ParseJSON(`[
		{"role":"assistant","blocks":[{"type":"thinking","text":"reason","signature":"sig"},{"type":"tool_use","tool_id":"same","name":"read_file","input":{}}]},
		{"role":"tool","blocks":[{"type":"tool_result","tool_id":"same","content":"ok","is_error":false}]},
		{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"same","name":"read_file","input":{}}]},
		{"role":"user","blocks":[{"type":"text","text":"next"}]},
		{"role":"tool","blocks":[{"type":"tool_result","tool_id":"orphan","content":"bad","is_error":false}]}
	]`)
	if n := MessageHistoryRepair(messages); n != 2 {
		t.Fatalf("repairs=%d, want 2", n)
	}
	if messages.Len() != 5 || messages.At(3).Get("blocks").At(0).GetString("tool_id") != "same" {
		t.Fatal("missing result was not inserted before next user turn")
	}
	if n := MessageHistoryRepair(messages); n != 0 {
		t.Fatalf("repair is not idempotent: %d", n)
	}
	if messages.At(0).Get("blocks").At(0).GetString("signature") != "sig" {
		t.Fatal("reasoning was changed")
	}
}

func TestCanonicalRepairHasNoLegacyCallOrNameTruncation(t *testing.T) {
	blocks := NewArray()
	for i := 0; i < 40; i++ {
		id := strings.Repeat("opaque-", 90) + string(rune('A'+i))
		call := mkCall(id, strings.Repeat("long_name_", 30), ParseJSON(`{"id":9007199254740993}`))
		blocks.Append(call.Get("blocks").At(0))
	}
	message := NewObject()
	message.Set("role", NewString("assistant"))
	message.Set("blocks", blocks)
	messages := NewArray()
	messages.Append(message)
	before := PrintJSONUnformatted(message)
	if repairs := MessageHistoryRepair(messages); repairs != 40 || messages.Len() != 41 {
		t.Fatalf("complete dynamic call group not repaired: repairs=%d count=%d", repairs, messages.Len())
	}
	if PrintJSONUnformatted(messages.At(0)) != before {
		t.Fatal("opaque input, identifiers or tool names changed")
	}
	for i := 0; i < 40; i++ {
		if messages.At(i+1).Get("blocks").At(0).GetString("tool_id") != blocks.At(i).GetString("tool_id") {
			t.Fatal("result correlation truncated")
		}
	}
}

func TestCanonicalTransformsRefuseNativeShapes(t *testing.T) {
	for _, raw := range []string{
		`[{"role":"user","content":"native"}]`,
		`[{"role":"assistant","content":[{"type":"tool_use","id":"a","name":"Read","input":{}}]}]`,
		`[{"type":"function_call_output","call_id":"a","output":"native"}]`,
	} {
		messages := ParseJSON(raw)
		original := PrintJSONUnformatted(messages)
		cfg := &FoldConfig{Enabled: true, RetainedMsgs: 1, MinFoldMsgs: 1}
		for _, result := range []FoldResult{FoldView(messages, cfg, nil), CompressView(messages, cfg)} {
			if result.Folded || result.Messages != nil {
				t.Fatal("native history entered canonical transform")
			}
		}
		if PrintJSONUnformatted(messages) != original {
			t.Fatal("native refusal changed input")
		}
	}
}
