package ir

import (
	"encoding/json"
	"strings"
	"testing"
)

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
