package ir

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"
	"testing"
)

func toolSourceBlock(t *testing.T) map[string]any {
	t.Helper()
	visible := "native source α 9007199254740993"
	hash := sha256.Sum256([]byte(visible))
	return map[string]any{"type": "tool_result", "tool_id": "call", "content": visible, "is_error": false, "tool_source": map[string]any{"schema_version": 1, "owner": "70000000-0000-4000-8000-000000000007", "visible_sha256": hex.EncodeToString(hash[:]), "receipt": "{\"signed\":\"opaque\"}"}}
}
func TestToolSourceExactVisibleContract(t *testing.T) {
	block := toolSourceBlock(t)
	raw, _ := json.Marshal(block)
	if !ValidBlock(raw) {
		t.Fatal("valid sidecar refused")
	}
	var decoded Block
	if !Decode(raw, &decoded) || decoded.ToolSource == nil {
		t.Fatal("source dropped")
	}
	for _, mutate := range []func(map[string]any){func(b map[string]any) { b["content"] = "changed" }, func(b map[string]any) { b["type"] = "text"; b["text"] = "changed" }, func(b map[string]any) { b["tool_source"] = nil }, func(b map[string]any) {
		b["tool_source"].(map[string]any)["owner"] = "00000000-0000-0000-0000-000000000000"
	}, func(b map[string]any) { b["tool_source"].(map[string]any)["extra"] = 1 }, func(b map[string]any) { b["tool_source"].(map[string]any)["receipt"] = "{\"x\":1,\"x\":2}" }, func(b map[string]any) { b["tool_source"].(map[string]any)["receipt"] = "{\"x\":\"\\u0000\"}" }, func(b map[string]any) { b["tool_source"].(map[string]any)["receipt"] = strings.Repeat("x", 524289) }} {
		b := toolSourceBlock(t)
		mutate(b)
		encoded, _ := json.Marshal(b)
		if ValidBlock(encoded) {
			t.Fatal("malformed or altered source admitted", string(encoded[:min(len(encoded), 500)]))
		}
	}
	// Duplicate outer members and wide textual source bytes stay exact, never JSON numbers.
	if ValidBlock(append(raw[:len(raw)-1], []byte(`,"tool_source":null}`)...)) {
		t.Fatal("duplicate source admitted")
	}
}

func TestToolSourceExplicitNullCannotNormalizeAwayAtHistoryOrRequestDecode(t *testing.T) {
	block := toolSourceBlock(t)
	message := map[string]any{"role": "tool", "blocks": []any{block}}
	request := map[string]any{"version": 1, "source_wire": "unspecified", "model": "model", "system": []any{}, "messages": []any{message}, "tools": []any{}, "stop_sequences": []any{}, "cache_policy": "native", "tool_choice": map[string]any{"mode": "unset"}}
	encoded, _ := json.Marshal([]any{message})
	if _, err := DecodeMessages(encoded); err != nil {
		t.Fatal("valid sourced baseline", err)
	}
	raw, _ := json.Marshal(request)
	if _, err := DecodeRequest(raw); err != nil {
		t.Fatal("valid request baseline", err)
	}
	block["tool_source"] = nil
	encoded, _ = json.Marshal([]any{message})
	if _, err := DecodeMessages(encoded); err == nil {
		t.Fatal("null source silently disappeared from history")
	}
	raw, _ = json.Marshal(request)
	if _, err := DecodeRequest(raw); err == nil {
		t.Fatal("null source silently disappeared from request")
	}
}
