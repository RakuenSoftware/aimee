package families

import (
	"bytes"
	"encoding/json"
	"io"
	"unicode/utf8"
)

// Validate the canonical contract, never a native provider protocol. Stateful
// checks (pending permissions, open blocks, ownership) remain the runtime's job;
// persisting a control response does not authorize its execution.
func validSessionIREvent(encoded []byte) bool {
	if len(encoded) > 1<<20 || !utf8.Valid(encoded) {
		return false
	}
	var e struct {
		Kind        string          `json:"kind"`
		MessageID   string          `json:"message_id,omitempty"`
		Role        string          `json:"role,omitempty"`
		RequestID   string          `json:"request_id,omitempty"`
		ControlKind string          `json:"control_kind,omitempty"`
		Payload     json.RawMessage `json:"payload"`
	}
	if !decodeIR(encoded, &e) {
		return false
	}
	switch e.Kind {
	case "history":
		return validIRHistoryEvent(e.Payload)
	case "message":
		var m struct {
			Role   string            `json:"role"`
			Blocks []json.RawMessage `json:"blocks"`
		}
		if e.MessageID == "" || !decodeIR(e.Payload, &m) || m.Role == "" || m.Blocks == nil || len(m.Blocks) > 4096 {
			return false
		}
		for _, b := range m.Blocks {
			if !validIRBlock(b) {
				return false
			}
		}
		return true
	case "delta":
		var d struct {
			Type            string          `json:"type"`
			BlockID         *int            `json:"block_id"`
			Kind            string          `json:"kind"`
			ToolID          string          `json:"tool_id"`
			Name            string          `json:"name"`
			Text            *string         `json:"text_delta"`
			Arguments       *string         `json:"tool_args_delta"`
			Signature       *string         `json:"signature_delta"`
			Redacted        *string         `json:"redacted_content"`
			Namespace       string          `json:"namespace"`
			Input           json.RawMessage `json:"input"`
			StopReason      string          `json:"stop_reason"`
			RawStopReason   string          `json:"raw_stop_reason"`
			UsageIn         *int64          `json:"usage_in"`
			UsageOut        *int64          `json:"usage_out"`
			UsageCacheRead  *int64          `json:"usage_cache_read"`
			UsageCacheWrite *int64          `json:"usage_cache_write"`
			UsageReasoning  *int64          `json:"usage_reasoning"`
			Error           string          `json:"error_message"`
		}
		if e.MessageID == "" || e.Role == "" || !decodeIR(e.Payload, &d) || (d.RawStopReason != "" && d.Type != "TURN_STOP") {
			return false
		}
		switch d.Type {
		case "TURN_START", "TURN_STOP", "ERROR":
			return (d.UsageIn == nil || *d.UsageIn >= 0) && (d.UsageOut == nil || *d.UsageOut >= 0) &&
				(d.UsageCacheRead == nil || *d.UsageCacheRead >= 0) &&
				(d.UsageCacheWrite == nil || *d.UsageCacheWrite >= 0) &&
				(d.UsageReasoning == nil || *d.UsageReasoning >= 0)
		case "BLOCK_START", "BLOCK_DELTA", "BLOCK_STOP":
			if d.BlockID == nil || *d.BlockID < 0 || *d.BlockID > 4095 || (d.Kind != "text" && d.Kind != "thinking" && d.Kind != "tool_use") {
				return false
			}
			if d.Type == "BLOCK_START" && d.Kind == "tool_use" && (d.ToolID == "" || d.Name == "") {
				return false
			}
			if d.Signature != nil && d.Kind != "thinking" {
				return false
			}
			if d.Redacted != nil && (d.Kind != "thinking" || d.Type != "BLOCK_START") {
				return false
			}
			if d.Namespace != "" && d.Kind != "tool_use" {
				return false
			}
			if d.Input != nil {
				var input map[string]json.RawMessage
				if d.Kind != "tool_use" || d.Type != "BLOCK_START" || !decodeIR(d.Input, &input) || input == nil {
					return false
				}
			}
			return (d.Text == nil || d.Kind == "text" || d.Kind == "thinking") && (d.Arguments == nil || (d.Kind == "tool_use" && d.Type == "BLOCK_DELTA"))
		default:
			return false
		}
	case "control_request", "control_response":
		var c map[string]json.RawMessage
		return e.RequestID != "" && e.ControlKind != "" && decodeIR(e.Payload, &c) && c != nil
	case "execution":
		var x struct {
			Status string `json:"status"`
			Error  string `json:"error,omitempty"`
		}
		if !decodeIR(e.Payload, &x) {
			return false
		}
		return x.Status == "running" || x.Status == "completed" || x.Status == "failed" || x.Status == "cancelled"
	default:
		return false
	}
}
func decodeIR(encoded []byte, out any) bool {
	if len(encoded) == 0 || bytes.Equal(bytes.TrimSpace(encoded), []byte("null")) {
		return false
	}
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.DisallowUnknownFields()
	if decoder.Decode(out) != nil {
		return false
	}
	return decoder.Decode(new(any)) == io.EOF
}
func irBlockKind(kind string) bool {
	switch kind {
	case "text", "thinking", "tool_use", "tool_result", "image", "document":
		return true
	default:
		return false
	}
}
func validIRBlock(encoded []byte) bool {
	var b struct {
		Type             string          `json:"type"`
		Text             *string         `json:"text"`
		Signature        *string         `json:"signature"`
		RedactedContent  *string         `json:"redacted_content"`
		ThinkingID       *string         `json:"thinking_id"`
		EncryptedContent *string         `json:"encrypted_content"`
		CacheControl     *string         `json:"cache_control"`
		CacheTTL         *string         `json:"cache_ttl"`
		MessageGroup     *int            `json:"message_group"`
		ToolID           string          `json:"tool_id"`
		Name             string          `json:"name"`
		Namespace        string          `json:"namespace"`
		Input            json.RawMessage `json:"input"`
		Content          json.RawMessage `json:"content"`
		IsError          *bool           `json:"is_error"`
		MediaType        string          `json:"media_type"`
		MediaRef         string          `json:"media_ref"`
		Context          *struct {
			Origin         int    `json:"origin"`
			Authority      int    `json:"authority"`
			Trust          int    `json:"trust"`
			Sensitivity    int    `json:"sensitivity"`
			ModelVisible   bool   `json:"model_visible"`
			RevisionDomain string `json:"revision_domain"`
			RevisionScope  string `json:"revision_scope"`
			RevisionEpoch  string `json:"revision_epoch"`
		} `json:"context"`
	}
	if !decodeIR(encoded, &b) || !irBlockKind(b.Type) || (b.MessageGroup != nil && (*b.MessageGroup < 1 || *b.MessageGroup > 4096)) {
		return false
	}
	switch b.Type {
	case "text", "thinking":
		return b.Text != nil
	case "tool_use":
		return b.ToolID != "" && b.Name != ""
	case "tool_result":
		return b.ToolID != ""
	case "image", "document":
		return b.MediaRef != ""
	default:
		return false
	}
}

// Avoid HTML-escaping a 1 MiB payload into a >3 MiB envelope. The browser receives
// JSON as data, never HTML. Page sizing and final serialization must use the same
// encoder, otherwise a bounded page can expand beyond its declared reply budget.
func encodeSessionIR(value any) ([]byte, error) {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(value); err != nil {
		return nil, err
	}
	return bytes.TrimSuffix(buffer.Bytes(), []byte("\n")), nil
}
