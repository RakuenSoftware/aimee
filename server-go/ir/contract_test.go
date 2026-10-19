package ir

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestNativeReplayProofContract(t *testing.T) {
	digest := strings.Repeat("a", 64)
	proof := map[string]any{"scope": digest, "block_digest": digest, "wire": "responses"}
	valid := func() bool {
		raw, err := json.Marshal(map[string]any{"kind": "replay_proof", "payload": proof})
		if err != nil {
			t.Fatal(err)
		}
		return ValidSessionEvent(raw)
	}
	if !valid() {
		t.Fatal("Responses proof rejected")
	}
	proof["thinking_binding"] = nil
	if valid() {
		t.Fatal("explicit null is not an absent native binding")
	}
	proof["wire"] = "anthropic"
	proof["thinking_binding"] = "ap1:" + digest + ":" + digest + ":" + digest
	if !valid() {
		t.Fatal("bound Anthropic proof rejected")
	}
	for _, field := range []string{"scope", "block_digest", "thinking_binding", "wire"} {
		original := proof[field]
		for _, bad := range []any{"invented", nil, 1, map[string]any{}} {
			proof[field] = bad
			if valid() {
				t.Fatalf("malformed %s accepted: %#v", field, bad)
			}
		}
		proof[field] = original
	}
	proof["prompt"] = "not metadata"
	if valid() {
		t.Fatal("unmodeled content in proof accepted")
	}
}

func TestRequestContractPreservesTypedCatalogAndOpaqueValues(t *testing.T) {
	raw := `{"version":1,"source_wire":"responses","cache_policy":"native","system":[],"messages":[{"role":"user","blocks":[{"type":"text","text":"question"}]}],"tools":[{"kind":"namespace","name":"functions","strict":"unset","children":[{"kind":"function","name":"Read","schema":{"type":"object"},"strict":"null","defer_loading":false}]},{"kind":"hosted","provider":"responses","native_type":"web_search","native_options":{"opaque":9007199254740993},"strict":"unset"}],"tool_choice":{"mode":"tool","name":"Read"},"parallel_tool_calls":false,"max_tokens":77,"temperature":0.2,"top_p":0.9,"stream":false,"stop_sequences":["END"],"reasoning_effort":"high","output_format":{"kind":"json_schema","name":"answer","schema":{"type":"object"},"strict":"true"}}`
	request, err := DecodeRequest([]byte(raw))
	if err != nil {
		t.Fatal(err)
	}
	if request.Tools[0].Children[0].Strict != "null" || request.ToolChoice.Name != "Read" || *request.ParallelToolCalls || *request.MaxTokens != 77 {
		t.Fatal(request)
	}
	encoded, err := json.Marshal(request)
	if err != nil || !strings.Contains(string(encoded), "9007199254740993") {
		t.Fatalf("opaque numeric value changed: %s %v", encoded, err)
	}
	for _, bad := range []string{
		strings.Replace(raw, `"source_wire":"responses"`, `"source_wire":"invented"`, 1),
		strings.Replace(raw, `"role":"user"`, `"role":"invented"`, 1),
		strings.Replace(raw, `"max_tokens":77`, `"max_tokens":0`, 1),
		strings.Replace(raw, `"strict":"null"`, `"strict":true`, 1),
		strings.Replace(raw, `"type":"text","text":"question"`, `"type":"text","text":"question","raw":{"native":"bypass"}`, 1),
	} {
		if _, err := DecodeRequest([]byte(bad)); err == nil {
			t.Fatalf("accepted invalid request %s", bad)
		}
	}
}
func TestPolicyViewIsExplicitAndStrict(t *testing.T) {
	raw := `{"version":1,"view":"gateway_policy","messages":[{"role":"user"}],"tools":[{"kind":"namespace","name":"functions","children":[{"kind":"function","name":"exec_command"}]}],"last_user_text":"question"}`
	view, err := DecodePolicyView([]byte(raw))
	if err != nil || view.Tools[0].Children[0].Name != "exec_command" {
		t.Fatal(view, err)
	}
	for _, bad := range []string{
		`{"roles":["user"],"tools":["exec_command"]}`,
		strings.Replace(raw, `"kind":"function","name":"exec_command"`, `"kind":"function","name":"exec_command","schema":{"type":"object"}`, 1),
		strings.Replace(raw, `"role":"user"`, `"role":"invented"`, 1),
		strings.Replace(raw, `"view":"gateway_policy"`, `"view":"native_request"`, 1),
	} {
		if _, err := DecodePolicyView([]byte(bad)); err == nil {
			t.Fatalf("accepted invalid view %s", bad)
		}
	}
}

func TestThinkingReplayBindingContract(t *testing.T) {
	binding := "ap1:" + strings.Repeat("a", 64) + ":" + strings.Repeat("b", 64) + ":" + strings.Repeat("c", 64)
	encoded := []byte(`{"type":"thinking","text":"reason","thinking_binding":"` + binding + `"}`)
	if !ValidBlock(encoded) {
		t.Fatal("refused canonical replay binding")
	}
	var block Block
	if !Decode(encoded, &block) || block.ThinkingBinding == nil || *block.ThinkingBinding != binding {
		t.Fatal(block)
	}
	for _, bad := range []string{strings.Replace(string(encoded), "ap1:", "ap2:", 1), strings.Replace(string(encoded), `"thinking"`, `"text"`, 1), strings.Replace(string(encoded), binding, binding+"a", 1)} {
		if ValidBlock([]byte(bad)) {
			t.Fatal("accepted malformed binding", bad)
		}
	}
}

func TestDuplicateJSONMembersRefuse(t *testing.T) {
	for _, raw := range []string{`{"version":1,"version":2}`, `{"input":{"a":1,"a":2}}`, `{"a":1,"\u0061":2}`} {
		var value any
		if Decode([]byte(raw), &value) {
			t.Fatalf("accepted ambiguous JSON: %s", raw)
		}
	}
	var value any
	if !Decode([]byte(`{"a":{"n":9007199254740993},"b":{"n":2}}`), &value) {
		t.Fatal("refused independent object keys")
	}
}

func TestCanonicalMessagesContract(t *testing.T) {
	raw := `[{"role":"assistant","blocks":[{"type":"thinking","text":"private","signature":"sig"},{"type":"tool_use","tool_id":"opaque","name":"Read","input":{"id":9007199254740993}}]},{"role":"user","blocks":[{"type":"tool_result","tool_id":"opaque","content":{"value":0.12345678901234567890123456789}}]},{"role":"user","content_scalar":true,"_compaction_boundary":true,"blocks":[{"type":"text","text":"continue"}]}]`
	messages, err := DecodeMessages([]byte(raw))
	if err != nil || !messages[2].ContentScalar || !messages[2].Boundary || string(messages[0].Blocks[1].Input) != `{"id":9007199254740993}` {
		t.Fatal(messages, err)
	}
	for _, bad := range []string{`null`, `{}`, `[{"role":"user","content":"native"}]`, strings.Replace(raw, `"role":"assistant"`, `"role":"user"`, 1), strings.Replace(raw, `"id":9007199254740993`, `"id":1,"id":2`, 1)} {
		if _, err := DecodeMessages([]byte(bad)); err == nil {
			t.Fatal("accepted invalid canonical history", bad)
		}
	}
}

func TestJSONStringsRetainUnicodeScalars(t *testing.T) {
	for _, raw := range []string{`{"x":"\ud83d\ude80"}`, `{"🚀":"rocket"}`, `{"x":"literal \\ud800"}`} {
		if !ValidJSON([]byte(raw)) {
			t.Fatal("valid Unicode refused", raw)
		}
	}
	for _, raw := range []string{`{"x":"\ud800"}`, `{"x":"\udc00"}`, `{"x":"\ud800x"}`, `{"\u0000":1}`, `{"x":"\u0000"}`} {
		if ValidJSON([]byte(raw)) {
			t.Fatal("lossy native string admitted", raw)
		}
	}
}

func TestNativeCheckpointContract(t *testing.T) {
	proof := map[string]any{"scope": strings.Repeat("a", 64), "context_digest": strings.Repeat("b", 64),
		"prefix_digest": strings.Repeat("c", 64), "harness": "codex", "native_id": "opaque:native", "claim": strings.Repeat("d", 32), "message_count": 2}
	valid := func() bool {
		raw, err := json.Marshal(map[string]any{"kind": "native_checkpoint", "payload": proof})
		if err != nil {
			t.Fatal(err)
		}
		return ValidSessionEvent(raw)
	}
	if !valid() {
		t.Fatal("valid checkpoint rejected")
	}
	for key, values := range map[string][]any{
		"scope": {nil, "bad", strings.Repeat("A", 64)}, "context_digest": {nil, "", 42}, "prefix_digest": {nil, strings.Repeat("x", 64)},
		"claim": {nil, strings.Repeat("a", 31), strings.Repeat("A", 32)}, "harness": {nil, "invented"},
		"native_id": {nil, "", strings.Repeat("x", 1025)}, "message_count": {nil, 0, -1, 1.5, 4097, "2"},
	} {
		original := proof[key]
		delete(proof, key)
		if valid() {
			t.Fatalf("missing %s accepted", key)
		}
		for _, bad := range values {
			proof[key] = bad
			if valid() {
				t.Fatalf("bad %s accepted: %#v", key, bad)
			}
		}
		proof[key] = original
	}
	proof["unmodeled_prompt"] = "must not enter metadata"
	if valid() {
		t.Fatal("unknown field accepted")
	}
	delete(proof, "unmodeled_prompt")
	proof["harness"] = "claude"
	proof["native_id"] = strings.Repeat("x", 1024)
	if !valid() {
		t.Fatal("valid boundary values refused")
	}
}
