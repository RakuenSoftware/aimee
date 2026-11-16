package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"strconv"
	"strings"
	"unicode/utf8"
)

// This is presentation/transport metadata. Neither an operation ID nor spans
// attest who authored a message; the downstream owner retains that decision.
type originalInputMetadata struct {
	OperationUUID  string
	ProtectedSpans json.RawMessage
}

func canonicalOperationUUID(s string) bool {
	if len(s) != 36 {
		return false
	}
	nonzero := false
	for i, c := range []byte(s) {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
			continue
		}
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
		nonzero = nonzero || c != '0'
	}
	return nonzero
}
func originalInputSessionID(s string) bool {
	if len(s) > 127 || !strings.HasPrefix(s, "web-") {
		return false
	}
	tail := strings.TrimPrefix(s, "web-")
	if canonicalOperationUUID(tail) {
		return true
	}
	parts := strings.Split(tail, "-")
	if len(parts) != 2 {
		return false
	}
	for _, part := range parts {
		if part == "" {
			return false
		}
		for _, c := range []byte(part) {
			if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'z') {
				return false
			}
		}
	}
	return true
}
func originalInputTransport(message, sid string, operation, spans json.RawMessage) (*originalInputMetadata, error) {
	if !originalInputSessionID(sid) {
		return nil, fmt.Errorf("original input requires an opaque web session ID")
	}
	if len(message) == 0 || len(message) > 262144 || !utf8.ValidString(message) || strings.IndexByte(message, 0) >= 0 {
		return nil, fmt.Errorf("invalid original input text")
	}
	var id string
	if len(operation) == 0 || json.Unmarshal(operation, &id) != nil || !canonicalOperationUUID(id) {
		return nil, fmt.Errorf("original_input_operation_uuid must be a canonical nonzero UUID")
	}
	if spans != nil {
		if !canonical.ValidJSON(spans) || bytes.Equal(bytes.TrimSpace(spans), []byte("null")) {
			return nil, fmt.Errorf("invalid protected_spans")
		}
		var values []json.RawMessage
		if json.Unmarshal(spans, &values) != nil || len(values) > 32 {
			return nil, fmt.Errorf("invalid protected_spans")
		}
		end := 0
		for _, raw := range values {
			var keys map[string]json.RawMessage
			if json.Unmarshal(raw, &keys) != nil || len(keys) != 2 || keys["offset"] == nil || keys["length"] == nil {
				return nil, fmt.Errorf("invalid protected span shape")
			}
			var offset, length int
			if json.Unmarshal(keys["offset"], &offset) != nil || json.Unmarshal(keys["length"], &length) != nil || offset < end || length < 1 || length > 8192 || offset < 0 || offset > len(message) || length > len(message)-offset {
				return nil, fmt.Errorf("invalid protected span bounds")
			}
			stop := offset + length
			if offset < len(message) && !utf8.RuneStart(message[offset]) || stop < len(message) && !utf8.RuneStart(message[stop]) {
				return nil, fmt.Errorf("protected spans must follow UTF-8 boundaries")
			}
			end = stop
		}
	}
	return &originalInputMetadata{OperationUUID: id, ProtectedSpans: spans}, nil
}

// A downstream committed User event supplies presentation text only; this
// helper neither creates an origin claim nor changes the transported event.
func committedUserText(line []byte, sid, operationUUID string) (string, bool) {
	if !canonical.ValidJSON(line) {
		return "", false
	}
	var envelope struct {
		Version   int             `json:"version"`
		SessionID string          `json:"session_id"`
		Sequence  string          `json:"sequence"`
		EventID   string          `json:"event_id"`
		TurnID    string          `json:"turn_id"`
		Actor     string          `json:"actor"`
		Event     json.RawMessage `json:"event"`
	}
	if json.Unmarshal(line, &envelope) != nil || envelope.Version != 1 || envelope.SessionID != sid || envelope.EventID == "" || envelope.TurnID == "" || envelope.Actor == "" || !canonical.ValidSessionEvent(envelope.Event) {
		return "", false
	}
	n, e := strconv.ParseUint(envelope.Sequence, 10, 64)
	if e != nil || n == 0 || strconv.FormatUint(n, 10) != envelope.Sequence {
		return "", false
	}
	var event struct {
		Kind    string            `json:"kind"`
		Payload canonical.Message `json:"payload"`
	}
	if json.Unmarshal(envelope.Event, &event) != nil || event.Kind != "message" || event.Payload.Role != "user" {
		return "", false
	}
	if len(event.Payload.Blocks) != 1 {
		return "", false
	}
	block := event.Payload.Blocks[0]
	if block.Type != "text" || block.Text == nil || block.InputAttachment == nil || block.InputAttachment.OperationUUID != operationUUID {
		return "", false
	}
	// ValidSessionEvent above validates the closed attachment and its text hash.
	return *block.Text, true
}
