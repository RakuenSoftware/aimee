package memory

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"unicode/utf8"
)

// ContextLimits is separate from legacy zero-means-default budget fields.
// A present zero is literal. Token limits remain explicitly unsupported until
// provider-bound counting can include the complete request and its reserves.
type ContextLimits struct {
	SchemaVersion          int  `json:"schema_version"`
	MaxContextBytes        *int `json:"max_context_bytes,omitempty"`
	MaxContextTokens       *int `json:"max_context_tokens,omitempty"`
	MaxRequestTokens       *int `json:"max_request_tokens,omitempty"`
	ReservedResponseTokens *int `json:"reserved_response_tokens,omitempty"`
	ReservedToolTokens     *int `json:"reserved_tool_tokens,omitempty"`
}

func (l *ContextLimits) UnmarshalJSON(raw []byte) error {
	// A hard limit must not disappear through JSON's last-key-wins, null or
	// case-insensitive struct matching. Decode exact field names once each.
	if !utf8.Valid(raw) {
		return fmt.Errorf("context limits require valid UTF-8")
	}
	var value ContextLimits
	decoder := json.NewDecoder(bytes.NewReader(raw))
	start, err := decoder.Token()
	if err != nil || start != json.Delim('{') {
		return fmt.Errorf("context limits require an object")
	}
	seen := make(map[string]bool, 6)
	for decoder.More() {
		token, err := decoder.Token()
		if err != nil {
			return err
		}
		key, ok := token.(string)
		if !ok || seen[key] {
			return fmt.Errorf("duplicate context limit field")
		}
		seen[key] = true
		var field json.RawMessage
		if err := decoder.Decode(&field); err != nil {
			return err
		}
		if bytes.Equal(field, []byte("null")) {
			return fmt.Errorf("null context limit is not absence")
		}
		var target any
		switch key {
		case "schema_version":
			target = &value.SchemaVersion
		case "max_context_bytes":
			target = &value.MaxContextBytes
		case "max_context_tokens":
			target = &value.MaxContextTokens
		case "max_request_tokens":
			target = &value.MaxRequestTokens
		case "reserved_response_tokens":
			target = &value.ReservedResponseTokens
		case "reserved_tool_tokens":
			target = &value.ReservedToolTokens
		default:
			return fmt.Errorf("unknown context limit field")
		}
		if err := json.Unmarshal(field, target); err != nil {
			return err
		}
	}
	if end, err := decoder.Token(); err != nil || end != json.Delim('}') {
		return fmt.Errorf("unterminated context limits")
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return fmt.Errorf("trailing context limits data")
	}
	*l = value
	return nil
}

type contextBudgetError struct{ kind, message string }

func (e *contextBudgetError) Error() string { return e.message }

func (l *ContextLimits) byteLimit(inherited int) (int, error) {
	if inherited < 0 {
		return 0, &contextBudgetError{"invalid_argument", "inherited context byte limit cannot be negative"}
	}
	if l == nil {
		if inherited > maxDataBody {
			return 0, &contextBudgetError{"invalid_argument", "context byte limit exceeds memory message capacity"}
		}
		return inherited, nil
	}
	if l.SchemaVersion != 1 {
		return 0, &contextBudgetError{"unsupported_version", "context_limits requires schema_version=1"}
	}
	for _, value := range []*int{l.MaxContextBytes, l.MaxContextTokens, l.MaxRequestTokens, l.ReservedResponseTokens, l.ReservedToolTokens} {
		if value != nil && *value < 0 {
			return 0, &contextBudgetError{"invalid_argument", "context limits and reserves cannot be negative"}
		}
	}
	if l.MaxContextTokens != nil || l.MaxRequestTokens != nil || l.ReservedResponseTokens != nil || l.ReservedToolTokens != nil {
		return 0, &contextBudgetError{"unsupported_mode", "provider-bound token counting and reserves are unavailable"}
	}
	limit := inherited
	if l.MaxContextBytes != nil {
		if *l.MaxContextBytes > maxDataBody {
			return 0, &contextBudgetError{"invalid_argument", "context byte limit exceeds memory message capacity"}
		}
		// The host/operator allocation is a ceiling. A request can narrow it,
		// including to literal zero, but cannot grant itself more context.
		limit = min(limit, *l.MaxContextBytes)
	}
	if limit > maxDataBody {
		return 0, &contextBudgetError{"invalid_argument", "context byte limit exceeds memory message capacity"}
	}
	return limit, nil
}

type ContextAccounting struct {
	SchemaVersion   int    `json:"schema_version"`
	Boundary        string `json:"boundary"`
	Unit            string `json:"unit"`
	CountState      string `json:"count_state"`
	TokenCountState string `json:"token_count_state"`
	MaxContextBytes int    `json:"max_context_bytes"`
	RenderedBytes   int    `json:"rendered_bytes"`
	Digest          string `json:"digest"`
}

func accountMemoryEnvelope(envelope string, limit int) (ContextAccounting, error) {
	if len(envelope) > limit {
		return ContextAccounting{}, &contextBudgetError{"context_budget_overflow", "serialized memory envelope exceeds its byte limit"}
	}
	return ContextAccounting{SchemaVersion: 1, Boundary: "memory_envelope", Unit: "utf8_bytes", CountState: "exact",
		TokenCountState: "unavailable", MaxContextBytes: limit, RenderedBytes: len(envelope),
		Digest: fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(envelope)))}, nil
}
