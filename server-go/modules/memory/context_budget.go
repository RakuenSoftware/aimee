package memory

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
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
	type wire ContextLimits
	var value wire
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&value); err != nil {
		return err
	}
	*l = ContextLimits(value)
	return nil
}

type contextBudgetError struct{ kind, message string }

func (e *contextBudgetError) Error() string { return e.message }

func (l *ContextLimits) byteLimit(inherited int) (int, error) {
	if l == nil {
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
		limit = *l.MaxContextBytes
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
