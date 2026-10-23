package providers

// This is the fixed availability-probe boundary, not a general model codec.
// Native bytes remain inert; only explicit observed terminals can grant health.
import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"strconv"
	"strings"
)

const probePrompt = "Reply with exactly: ok"

// Existing C context ABI: aimee_context_{origin,authority,trust}_t.
// These are code-owned literals and untrusted diagnostic observations, never
// authenticated user/actor provenance or a credential capability.
const (
	probePlatform        = 1
	probeModel           = 7
	probeDiagnostic      = 0
	probeTaskInstruction = 2
	probePolicy          = 3
	probeUntrusted       = 1
	probeVerified        = 4
)

var errProbe = errors.New("provider returned no complete canonical diagnostic answer")

func probeRequest(model, wire string) *canonical.Request {
	prompt := probePrompt
	r := &canonical.Request{Version: 1, Model: model, SourceWire: wire, CachePolicy: "native",
		System: []canonical.Block{}, Messages: []canonical.Message{{Role: "user", ContentScalar: true, Blocks: []canonical.Block{{Type: "text", Text: &prompt, Context: &canonical.Context{Origin: probePlatform, Authority: probeTaskInstruction, Trust: probeVerified, ModelVisible: true}}}}},
		Tools: []canonical.Tool{}, Stops: []string{}, ToolChoice: canonical.ToolChoice{Mode: "unset"}}
	if wire == "responses" {
		policy := "You are performing a bounded availability probe."
		r.System = []canonical.Block{{Type: "text", Text: &policy, Context: &canonical.Context{Origin: probePlatform, Authority: probePolicy, Trust: probeVerified, ModelVisible: true}}}
		r.Stream = true
	} else if wire != "unspecified" {
		n := 256
		r.MaxTokens = &n
	}
	return r
}
func probeWire(r *canonical.Request, localThinking bool) ([]byte, error) {
	if !r.Valid() || len(r.Messages) != 1 || len(r.Messages[0].Blocks) != 1 || r.Messages[0].Blocks[0].Text == nil {
		return nil, errProbe
	}
	owned, _ := json.Marshal(r)
	fixed, _ := json.Marshal(probeRequest(r.Model, r.SourceWire))
	if !bytes.Equal(owned, fixed) {
		return nil, errProbe
	}
	text := *r.Messages[0].Blocks[0].Text
	body := object{"model": r.Model, "messages": []object{{"role": "user", "content": text}}, "max_tokens": r.MaxTokens}
	if r.SourceWire == "responses" {
		body = object{"model": r.Model, "store": false, "stream": r.Stream, "instructions": *r.System[0].Text, "input": []object{{"type": "message", "role": "user", "content": []object{{"type": "input_text", "text": text}}}}}
	} else if r.SourceWire != "openai_chat" && r.SourceWire != "anthropic" {
		return nil, errProbe
	}
	// Preserve the selected local provider's existing egress-only profile extension.
	if localThinking && r.SourceWire != "responses" {
		body["chat_template_kwargs"] = object{"enable_thinking": false}
	}
	return json.Marshal(body)
}
func probeObject(raw []byte) (object, error) {
	if !canonical.ValidJSON(raw) {
		return nil, errProbe
	}
	var v object
	d := json.NewDecoder(bytes.NewReader(raw))
	d.UseNumber()
	if d.Decode(&v) != nil || v == nil {
		return nil, errProbe
	}
	return v, nil
}
func probeMap(v any) object { m, _ := v.(map[string]any); return m }
func probeResponseBase(raw []byte) *canonical.Response {
	role := "assistant"
	sidecar, _ := json.Marshal(object{"native_body_base64": base64.StdEncoding.EncodeToString(raw)})
	return &canonical.Response{Schema: "aimee.response.v1", Role: &role, Blocks: []canonical.ResponseBlock{}, Usage: canonical.ZeroUsage(), Raw: sidecar}
}
func probeStop(r *canonical.Response, raw string, wire string) {
	r.RawStopReason = &raw
	r.StopReason = canonical.StopUnknown
	switch raw {
	case "stop", "end_turn", "completed":
		r.StopReason = canonical.StopEndTurn
	case "stop_sequence":
		r.StopReason = canonical.StopSequence
	case "length", "max_tokens":
		r.StopReason = canonical.StopMaxTokens
	case "tool_calls", "function_call", "tool_use":
		r.StopReason = canonical.StopToolUse
	case "content_filter", "refusal":
		r.StopReason = canonical.StopContentFilter
	case "error", "failed", "cancelled":
		r.StopReason = canonical.StopError
	}
	// A stop spelling valid in another protocol is not an implicit terminal.
	if wire == "openai_chat" && raw != "stop" && raw != "length" && raw != "tool_calls" && raw != "function_call" && raw != "content_filter" {
		r.StopReason = canonical.StopUnknown
	}
	if wire == "anthropic" && raw != "end_turn" && raw != "stop_sequence" && raw != "max_tokens" && raw != "tool_use" && raw != "refusal" {
		r.StopReason = canonical.StopUnknown
	}
}
func probeText(r *canonical.Response, kind, text string) {
	r.Blocks = append(r.Blocks, canonical.ResponseBlock{Block: canonical.Block{Type: kind, Text: &text, Context: &canonical.Context{Origin: probeModel, Authority: probeDiagnostic, Trust: probeUntrusted, ModelVisible: true}}})
}
func probeParts(r *canonical.Response, value any) error {
	if value == nil {
		return nil
	}
	if text, ok := value.(string); ok {
		probeText(r, "text", text)
		return nil
	}
	list, ok := value.([]any)
	if !ok {
		return errProbe
	}
	for _, v := range list {
		p := probeMap(v)
		switch str(p, "type") {
		case "text", "output_text":
			text, ok := p["text"].(string)
			if !ok {
				return errProbe
			}
			probeText(r, "text", text)
		case "thinking", "reasoning_text":
			text, ok := p["thinking"].(string)
			if !ok {
				text, ok = p["text"].(string)
			}
			if !ok {
				return errProbe
			}
			probeText(r, "thinking", text)
			if value := p["signature"]; value != nil {
				signature, ok := value.(string)
				if !ok {
					return errProbe
				}
				r.Blocks[len(r.Blocks)-1].Signature = &signature
			}
		case "redacted_thinking":
			data, ok := p["data"].(string)
			if !ok {
				return errProbe
			}
			probeText(r, "thinking", "")
			r.Blocks[len(r.Blocks)-1].RedactedContent = &data
		case "refusal":
			r.StopReason = canonical.StopContentFilter
		default:
			return errProbe
		}
	}
	return nil
}
func probeUsage(r *canonical.Response, u object) error {
	for _, key := range []string{"prompt_tokens_details", "input_tokens_details", "completion_tokens_details", "output_tokens_details"} {
		if value := u[key]; value != nil && probeMap(value) == nil {
			return errProbe
		}
	}
	fields := []struct {
		value any
		out   *string
	}{
		{u["input_tokens"], &r.Usage.Input}, {u["prompt_tokens"], &r.Usage.Input},
		{u["output_tokens"], &r.Usage.Output}, {u["completion_tokens"], &r.Usage.Output},
		{u["cache_read_input_tokens"], &r.Usage.CacheRead}, {u["cached_input_tokens"], &r.Usage.CacheRead},
		{probeMap(u["prompt_tokens_details"])["cached_tokens"], &r.Usage.CacheRead},
		{probeMap(u["input_tokens_details"])["cached_tokens"], &r.Usage.CacheRead},
		{u["cache_creation_input_tokens"], &r.Usage.CacheWrite},
		{probeMap(u["completion_tokens_details"])["reasoning_tokens"], &r.Usage.Reasoning},
		{probeMap(u["output_tokens_details"])["reasoning_tokens"], &r.Usage.Reasoning},
	}
	for _, field := range fields {
		if field.value == nil {
			continue
		}
		n, ok := field.value.(json.Number)
		if !ok {
			return errProbe
		}
		i, e := strconv.ParseInt(string(n), 10, 64)
		if e != nil || i < 0 {
			return errProbe
		}
		*field.out = string(n)
	}
	return nil
}
func probeMetadata(r *canonical.Response, m object) error {
	for key, dst := range map[string]**string{"id": &r.ID, "model": &r.Model} {
		if value := m[key]; value != nil {
			text, ok := value.(string)
			if !ok {
				return errProbe
			}
			*dst = &text
		}
	}
	if value := m["usage"]; value != nil && probeMap(value) == nil {
		return errProbe
	}
	return probeUsage(r, probeMap(m["usage"]))
}
func probeMessage(r *canonical.Response, m object) error {
	if m == nil || m["error"] != nil {
		return errProbe
	}
	if value, present := m["role"]; present && value != nil {
		role, ok := value.(string)
		if !ok || role != "assistant" {
			return errProbe
		}
	}
	if v := m["refusal"]; v != nil {
		if text, ok := v.(string); !ok || text != "" {
			r.StopReason = canonical.StopContentFilter
		}
	}
	if v := m["tool_calls"]; v != nil {
		calls, ok := v.([]any)
		if !ok || len(calls) > 0 {
			return errProbe
		}
	}
	if v := m["function_call"]; v != nil {
		return errProbe
	}
	if value := m["reasoning_content"]; value != nil {
		text, ok := value.(string)
		if !ok {
			return errProbe
		}
		probeText(r, "thinking", text)
	}
	return probeParts(r, m["content"])
}
func probeHTTPResponse(body []byte, wire string) (*canonical.Response, error) {
	if wire == "responses" {
		return probeResponsesStream(body)
	}
	r := probeResponseBase(body)
	root, err := probeObject(body)
	if err != nil {
		return r, err
	}
	if root["error"] != nil {
		r.StopReason = canonical.StopError
		return r, errProbe
	}
	if wire == "openai_chat" {
		choices, ok := root["choices"].([]any)
		if !ok || len(choices) != 1 || probeMap(choices[0]) == nil {
			return r, errProbe
		}
		c := probeMap(choices[0])
		probeStop(r, str(c, "finish_reason"), wire)
		err = probeMessage(r, probeMap(c["message"]))
	} else if wire == "anthropic" {
		probeStop(r, str(root, "stop_reason"), wire)
		err = probeMessage(r, root)
	} else {
		return r, errProbe
	}
	if err == nil {
		err = probeMetadata(r, root)
	}
	if err != nil || !r.Valid() {
		return r, errProbe
	}
	return r, nil
}
func probeResponseOutput(r *canonical.Response, items []object) error {
	for _, item := range items {
		switch str(item, "type") {
		case "message":
			if err := probeMessage(r, item); err != nil {
				return err
			}
		case "reasoning": // Probe reasoning is inert; the complete native item stays in Raw.
		default:
			return errProbe
		}
	}
	return nil
}
func probeResponsesStream(body []byte) (*canonical.Response, error) {
	r := probeResponseBase(body)
	scanner := bufio.NewScanner(bytes.NewReader(body))
	scanner.Buffer(make([]byte, 4096), 1<<20)
	done := false
	seenID := ""
	var parts []object
	var completed object
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		data := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if data == "[DONE]" {
			continue
		}
		event, e := probeObject([]byte(data))
		if e != nil {
			return r, e
		}
		if value := event["response_id"]; value != nil {
			id, ok := value.(string)
			if !ok || id == "" {
				return r, errProbe
			}
			if seenID != "" && id != seenID {
				return r, errProbe
			}
			seenID = id
		}
		if event["error"] != nil {
			return r, errProbe
		}
		eventType := str(event, "type")
		if done && strings.HasPrefix(eventType, "response.") {
			return r, errProbe
		}
		if strings.HasPrefix(eventType, "response.function_call") {
			return r, errProbe
		}
		switch str(event, "type") {
		case "error", "response.failed", "response.incomplete":
			r.StopReason = canonical.StopError
			return r, errProbe
		case "response.output_item.added":
			kind := str(probeMap(event["item"]), "type")
			if kind != "message" && kind != "reasoning" {
				return r, errProbe
			}
		case "response.content_part.added":
			kind := str(probeMap(event["part"]), "type")
			if kind != "output_text" {
				return r, errProbe
			}
		case "response.output_item.done":
			parts = append(parts, probeMap(event["item"]))
		case "response.output_text.done":
			parts = append(parts, object{"type": "message", "role": "assistant", "content": []any{object{"type": "output_text", "text": event["text"]}}})
		case "response.content_part.done":
			parts = append(parts, object{"type": "message", "role": "assistant", "content": []any{event["part"]}})
		case "response.completed":
			if done {
				return r, errProbe
			}
			done = true
			completed = probeMap(event["response"])
		}
	}
	if scanner.Err() != nil || !done || str(completed, "status") != "completed" || completed["error"] != nil {
		return r, errProbe
	}
	if id := str(completed, "id"); seenID != "" && id != seenID {
		return r, errProbe
	}
	observed := probeResponseBase(body)
	if probeResponseOutput(observed, parts) != nil || observed.StopReason == canonical.StopContentFilter {
		return r, errProbe
	}
	probeStop(r, "completed", "responses")
	var output []object
	if value := completed["output"]; value != nil {
		items, ok := value.([]any)
		if !ok {
			return r, errProbe
		}
		for _, item := range items {
			if probeMap(item) == nil {
				return r, errProbe
			}
			output = append(output, probeMap(item))
		}
	}
	if len(output) == 0 {
		output = parts
	}
	if probeResponseOutput(r, output) != nil || probeMetadata(r, completed) != nil || !r.Valid() {
		return r, errProbe
	}
	return r, nil
}
