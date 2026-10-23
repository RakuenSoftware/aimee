package providers

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"strings"
	"testing"
)

func TestProbePartialRefusedUnknownCannotBeHealthy(t *testing.T) {
	for _, body := range []string{
		`{"choices":[{"finish_reason":"length","message":{"role":"assistant","content":"partial"}}]}`,
		`{"choices":[{"finish_reason":"content_filter","message":{"role":"assistant","content":"refused"}}]}`,
		`{"choices":[{"finish_reason":"future","message":{"role":"assistant","content":"unknown"}}]}`,
		`{"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"text","refusal":"declined"}}]}`,
		`{"content":[{"type":"text","text":"partial"}],"stop_reason":"max_tokens"}`,
		`{"content":[{"type":"text","text":"refused"}],"stop_reason":"refusal"}`,
	} {
		wire := "openai_chat"
		if strings.Contains(body, "stop_reason") {
			wire = "anthropic"
		}
		if r, e := probeHTTPResponse([]byte(body), wire); e == nil && r.CompleteText() {
			t.Errorf("noncomplete diagnostic accepted: %s", body)
		}
	}
}
func TestProbeRequestIsOwnedBeforeWireProjection(t *testing.T) {
	for _, wire := range []string{"openai_chat", "anthropic", "responses"} {
		r := probeRequest("fixture-model", wire)
		if !r.Valid() {
			t.Fatal("invalid owned request", wire)
		}
		raw, e := probeWire(r, true)
		if e != nil {
			t.Fatal(e)
		}
		var out object
		if json.Unmarshal(raw, &out) != nil || str(out, "model") != "fixture-model" {
			t.Fatal(out)
		}
		if wire == "responses" {
			if out["max_tokens"] != nil || out["store"] != false || out["stream"] != true || out["chat_template_kwargs"] != nil {
				t.Fatal(out)
			}
		} else if number(out, "max_tokens") != 256 {
			t.Fatal(out)
		}
		r.Metadata = json.RawMessage(`{"unmodeled_control":true}`)
		if _, e := probeWire(r, false); e == nil {
			t.Fatal("silently dropped non-probe control")
		}
	}
}
func TestProbeCanonicalResponseRetainsFullRawAndUsage(t *testing.T) {
	body := []byte(`{"id":"r1","model":"served","unknown":{"future":true},"choices":[{"finish_reason":"stop","message":{"role":"assistant","reasoning_content":"private","content":"ok","tool_calls":[]}}],"usage":{"prompt_tokens":12,"completion_tokens":3,"prompt_tokens_details":{"cached_tokens":4}}}`)
	r, e := probeHTTPResponse(body, "openai_chat")
	if e != nil || !r.CompleteText() {
		t.Fatalf("%+v %v", r, e)
	}
	var sidecar struct {
		Body string `json:"native_body_base64"`
	}
	if json.Unmarshal(r.Raw, &sidecar) != nil {
		t.Fatal("missing raw")
	}
	decoded, e := base64.StdEncoding.DecodeString(sidecar.Body)
	if e != nil || !bytes.Equal(decoded, body) {
		t.Fatal("raw changed")
	}
	if r.Model == nil || *r.Model != "served" || r.ID == nil || *r.ID != "r1" || r.Usage.Input != "12" || r.Usage.Output != "3" || r.Usage.CacheRead != "4" || len(r.Blocks) != 2 || r.Blocks[0].Type != "thinking" {
		t.Fatalf("metadata lost %+v", r)
	}
}
func TestProbeRejectsAmbiguousAndMalformedModelObservations(t *testing.T) {
	cases := []struct{ wire, body string }{
		{"openai_chat", `{"choices":[{"finish_reason":"stop","message":{"role":"user","content":"ok"}}]}`},
		{"openai_chat", `{"choices":[{"finish_reason":"stop","message":{"role":42,"content":"ok"}}]}`},
		{"openai_chat", `{"choices":[{"finish_reason":"stop","message":{"reasoning_content":[],"content":"ok"}}]}`},
		{"openai_chat", `{"error":{},"choices":[{"finish_reason":"stop","message":{"content":"ok"}}]}`},
		{"openai_chat", `{"choices":[{"finish_reason":"stop","message":{"content":"ok","tool_calls":[{}]}}]}`},
		{"openai_chat", `{"choices":[{"finish_reason":"stop","message":{"content":"ok"}}],"usage":{"prompt_tokens":-1}}`},
		{"anthropic", `{"content":[{"type":"thinking","thinking":"only"}],"stop_reason":"end_turn"}`},
		{"anthropic", `{"content":[{"type":"tool_use","id":"t","name":"write_file","input":{}}],"stop_reason":"tool_use"}`},
		{"anthropic", `{"content":[{"type":"text","text":"ok"}],"stop_reason":"future"}`},
		{"openai_chat", `{"choices":[],"choices":[{"finish_reason":"stop","message":{"content":"ok"}}]}`},
	}
	for _, tc := range cases {
		r, e := probeHTTPResponse([]byte(tc.body), tc.wire)
		if e == nil && r.CompleteText() {
			t.Fatal("invalid observation healthy", tc.body)
		}
	}
	complete := `data: {"type":"response.completed","response":{"id":"owned","status":"completed","output":[]}}` + "\n\n"
	for _, stream := range []string{
		`data: {"type":"response.output_text.done","response_id":"foreign","text":"ok"}` + "\n\n" + complete,
		complete + `data: {"type":"response.output_text.done","text":"late"}` + "\n\n",
		`data: {"type":"response.output_item.done","item":{"type":"message","role":"assistant","content":[{"type":"refusal","refusal":"declined"}]}}` + "\n\n" + `data: {"type":"response.completed","response":{"status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}]}}` + "\n\n",
		`data: {"type":"response.output_item.done","item":{"type":"function_call","name":"write_file"}}` + "\n\n" + complete,
		`data: {"type":"response.output_item.added","item":{"type":"function_call","name":"write_file"}}` + "\n\n" + complete,
		`data: {"type":"response.content_part.added","part":{"type":"refusal","refusal":"declined"}}` + "\n\n" + complete,
	} {
		r, e := probeHTTPResponse([]byte(stream), "responses")
		if e == nil && r.CompleteText() {
			t.Fatal("foreign/late/tool stream healthy")
		}
	}
}

func TestProbeRejectsMalformedKnownUsage(t *testing.T) {
	for _, usage := range []string{`[]`, `"bad"`, `3`, `{"input_tokens":1.5}`, `{"input_tokens":-1}`, `{"input_tokens":9223372036854775808}`, `{"prompt_tokens_details":[]}`, `{"input_tokens_details":1}`, `{"completion_tokens_details":"bad"}`, `{"output_tokens_details":{"reasoning_tokens":-1}}`} {
		for _, wire := range []string{"openai_chat", "anthropic", "responses"} {
			var body string
			switch wire {
			case "openai_chat":
				body = `{"choices":[{"finish_reason":"stop","message":{"content":"ok"}}],"usage":` + usage + `}`
			case "anthropic":
				body = `{"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn","usage":` + usage + `}`
			case "responses":
				body = "data: " + `{"type":"response.completed","response":{"status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}],"usage":` + usage + `}}` + "\n\n"
			}
			r, e := probeHTTPResponse([]byte(body), wire)
			if e == nil && r.CompleteText() {
				t.Fatalf("malformed %s usage accepted: %s", wire, usage)
			}
			if len(r.Raw) == 0 {
				t.Fatal("lost original raw diagnostic")
			}
		}
	}
}

func TestExplicitResponsesRefusalCannotBeOverwrittenByCompletion(t *testing.T) {
	for _, event := range []string{"response.refusal.delta", "response.refusal.done"} {
		for _, id := range []string{"owned", "foreign"} {
			for _, output := range []string{`[]`, `[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}]`} {
				for _, late := range []bool{false, true} {
					refusal := "data: " + `{"type":"` + event + `","response_id":"` + id + `","delta":"declined","refusal":"declined"}` + "\n\n"
					completion := "data: " + `{"type":"response.completed","response":{"id":"owned","status":"completed","output":` + output + `}}` + "\n\n"
					body := refusal + completion
					if late {
						body = completion + refusal
					}
					r, e := probeHTTPResponse([]byte(body), "responses")
					if e == nil || r.CompleteText() {
						t.Fatalf("refusal granted health: %s id=%s late=%v output=%s", event, id, late, output)
					}
					if r.StopReason != canonical.StopContentFilter || r.RawStopReason == nil || *r.RawStopReason != "refusal" {
						t.Fatalf("explicit refusal lost: %+v", r)
					}
					var sidecar struct {
						Body string `json:"native_body_base64"`
					}
					if json.Unmarshal(r.Raw, &sidecar) != nil {
						t.Fatal("missing raw")
					}
					raw, err := base64.StdEncoding.DecodeString(sidecar.Body)
					if err != nil || !bytes.Equal(raw, []byte(body)) {
						t.Fatal("refusal raw changed")
					}
				}
			}
		}
	}
}

func TestResponsesClosedAliasesProjectOnceAndRejectConflicts(t *testing.T) {
	event := func(value string) string { return "data: " + value + "\n\n" }
	complete := event(`{"type":"response.completed","response":{"status":"completed","output":[]}}`)
	item := func(id, text string) string {
		return event(`{"type":"response.output_item.done","item":{"id":"` + id + `","type":"message","role":"assistant","content":[{"type":"output_text","text":"` + text + `"}]}}`)
	}
	part := func(id, text string) string {
		return event(`{"type":"response.content_part.done","item_id":"` + id + `","part":{"type":"output_text","text":"` + text + `"}}`)
	}
	text := func(id, text string) string {
		return event(`{"type":"response.output_text.done","item_id":"` + id + `","text":"` + text + `"}`)
	}
	project := func(body string) (*canonical.Response, error) {
		return probeHTTPResponse([]byte(body+complete), "responses")
	}
	r, e := project(text("a", "ok") + part("a", "ok") + item("a", "ok"))
	if e != nil || !r.CompleteText() || len(r.Blocks) != 1 || *r.Blocks[0].Text != "ok" {
		t.Fatalf("overlapping aliases duplicated or lost: %+v %v", r, e)
	}
	r, e = project(item("a", "first") + item("b", "second") + part("a", "first") + part("b", "second") + text("a", "first") + text("b", "second"))
	if e != nil || !r.CompleteText() || len(r.Blocks) != 2 || *r.Blocks[0].Text != "first" || *r.Blocks[1].Text != "second" {
		t.Fatalf("distinct items lost: %+v %v", r, e)
	}
	for _, body := range []string{
		item("a", "ok") + part("a", "changed"), item("a", "ok") + text("foreign", "ok"),
		item("a", "first") + item("b", "second") + part("a", "firstsecond") + part("b", ""),
		item("a", "ok") + item("a", "ok"),
		event(`{"type":"response.output_item.done","output_index":0,"item":{"id":"a","type":"message","content":[{"type":"output_text","text":"ok"}]}}`) + event(`{"type":"response.output_text.done","output_index":1,"item_id":"a","text":"ok"}`),
		item("a", "ok") + event(`{"type":"response.output_text.done","item_id":"a","output_index":"bad","text":"ok"}`),
		item("a", "ok") + event(`{"type":"response.content_part.done","item_id":"a","part":{"type":"refusal","refusal":"declined"}}`),
		item("a", "ok") + event(`{"type":"response.output_item.done","item":{"type":"function_call","name":"write_file"}}`),
	} {
		r, e = project(body)
		if e == nil || r.CompleteText() {
			t.Fatal("conflicting or unsafe lower-level observation admitted", body)
		}
	}
}

func TestResponsesNestedCreatedIdentityMustMatchTerminal(t *testing.T) {
	for _, kind := range []string{"response.created", "response.in_progress"} {
		for _, terminal := range []string{"owned", "foreign"} {
			body := "data: " + `{"type":"` + kind + `","response":{"id":"owned","status":"in_progress"}}` + "\n\n" +
				"data: " + `{"type":"response.output_text.done","item_id":"message","output_index":0,"content_index":0,"text":"ok"}` + "\n\n" +
				"data: " + `{"type":"response.completed","response":{"id":"` + terminal + `","status":"completed","output":[]}}` + "\n\n"
			r, e := probeHTTPResponse([]byte(body), "responses")
			if terminal == "owned" {
				if e != nil || !r.CompleteText() {
					t.Fatal("standard identity rejected", e)
				}
			} else if e == nil || r.CompleteText() {
				t.Fatal("foreign nested terminal identity accepted")
			}
		}
	}
}
