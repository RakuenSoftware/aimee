package ir

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"
	"unicode/utf8"
)

// Tool is a catalog definition, independent of any native function envelope.
// Schemas and explicitly provider-owned options remain opaque JSON values.
type Tool struct {
	Kind          string          `json:"kind"`
	Name          string          `json:"name,omitempty"`
	Description   string          `json:"description,omitempty"`
	Schema        json.RawMessage `json:"schema,omitempty"`
	Strict        string          `json:"strict,omitempty"`
	DeferLoading  *bool           `json:"defer_loading,omitempty"`
	CacheControl  string          `json:"cache_control,omitempty"`
	CacheTTL      string          `json:"cache_ttl,omitempty"`
	Provider      string          `json:"provider,omitempty"`
	NativeType    string          `json:"native_type,omitempty"`
	NativeOptions json.RawMessage `json:"native_options,omitempty"`
	Children      []Tool          `json:"children,omitempty"`
}
type ToolChoice struct {
	Mode string `json:"mode"`
	Name string `json:"name,omitempty"`
}
type OutputFormat struct {
	Kind        string          `json:"kind"`
	Name        string          `json:"name,omitempty"`
	Description string          `json:"description,omitempty"`
	Schema      json.RawMessage `json:"schema,omitempty"`
	Strict      string          `json:"strict"`
}
type ProviderConfig struct {
	Wire     string          `json:"wire"`
	Thinking json.RawMessage `json:"thinking,omitempty"`
}
type Request struct {
	Version           int             `json:"version"`
	Model             string          `json:"model,omitempty"`
	SourceWire        string          `json:"source_wire"`
	CachePolicy       string          `json:"cache_policy"`
	System            []Block         `json:"system"`
	Messages          []Message       `json:"messages"`
	Tools             []Tool          `json:"tools"`
	ToolChoice        ToolChoice      `json:"tool_choice"`
	ParallelToolCalls *bool           `json:"parallel_tool_calls,omitempty"`
	MaxTokens         *int            `json:"max_tokens,omitempty"`
	Temperature       *float64        `json:"temperature,omitempty"`
	TopP              *float64        `json:"top_p,omitempty"`
	TopK              *int            `json:"top_k,omitempty"`
	Stream            bool            `json:"stream"`
	Stops             []string        `json:"stop_sequences"`
	Metadata          json.RawMessage `json:"metadata,omitempty"`
	ServiceTier       string          `json:"service_tier,omitempty"`
	ReasoningEffort   string          `json:"reasoning_effort,omitempty"`
	ReasoningSummary  string          `json:"reasoning_summary,omitempty"`
	ProviderConfig    *ProviderConfig `json:"provider_config,omitempty"`
	OutputFormat      *OutputFormat   `json:"output_format,omitempty"`
}

func validStrict(value string) bool {
	return value == "" || value == "unset" || value == "null" || value == "true" || value == "false"
}
func object(value json.RawMessage) bool {
	return len(value) > 0 && ValidJSON(value) && strings.HasPrefix(strings.TrimSpace(string(value)), "{")
}
func (t Tool) Valid() bool {
	if !validStrict(t.Strict) || (t.Schema != nil && !object(t.Schema)) || (t.NativeOptions != nil && !object(t.NativeOptions)) {
		return false
	}
	switch t.Kind {
	case "function":
		return t.Name != "" && len(t.Children) == 0 && t.Provider == "" && t.NativeType == "" && t.NativeOptions == nil
	case "hosted":
		return t.NativeType != "" && (t.Provider == "responses" || t.Provider == "anthropic" && t.Name != "") && len(t.Children) == 0
	case "namespace":
		if t.Name == "" || len(t.Children) > 4096 {
			return false
		}
		for _, child := range t.Children {
			if child.Kind != "function" || !child.Valid() {
				return false
			}
		}
		return true
	default:
		return false
	}
}
func validRole(role string) bool {
	switch role {
	case "system", "developer", "user", "assistant", "tool":
		return true
	}
	return false
}
func validTypedBlock(block Block) bool {
	encoded, err := json.Marshal(block)
	return err == nil && ValidBlock(encoded)
}
func (r *Request) Valid() bool {
	if r == nil || r.Version != 1 || r.System == nil || r.Messages == nil || r.Tools == nil || r.Stops == nil ||
		len(r.System) > 4096 || len(r.Messages) > 4096 || len(r.Tools) > 4096 || len(r.Stops) > 4096 {
		return false
	}
	switch r.SourceWire {
	case "unspecified", "anthropic", "openai_chat", "responses":
	default:
		return false
	}
	if r.CachePolicy != "native" && r.CachePolicy != "stable_prefix" {
		return false
	}
	for _, b := range r.System {
		if !validTypedBlock(b) || b.Type == "tool_use" || b.Type == "tool_result" {
			return false
		}
	}
	for _, message := range r.Messages {
		if !validRole(message.Role) || message.Blocks == nil || len(message.Blocks) > 4096 {
			return false
		}
		for _, block := range message.Blocks {
			if !validTypedBlock(block) || block.Type == "tool_use" && message.Role != "assistant" ||
				block.Type == "tool_result" && message.Role != "user" && message.Role != "tool" {
				return false
			}
		}
	}
	for _, tool := range r.Tools {
		if !tool.Valid() {
			return false
		}
	}
	switch r.ToolChoice.Mode {
	case "unset", "auto", "none", "any":
		if r.ToolChoice.Name != "" {
			return false
		}
	case "tool":
		if r.ToolChoice.Name == "" {
			return false
		}
	default:
		return false
	}
	if r.MaxTokens != nil && (*r.MaxTokens < 1 || *r.MaxTokens > math.MaxInt32) ||
		r.Temperature != nil && (math.IsNaN(*r.Temperature) || math.IsInf(*r.Temperature, 0)) ||
		r.TopP != nil && (math.IsNaN(*r.TopP) || math.IsInf(*r.TopP, 0)) {
		return false
	}
	switch r.ReasoningEffort {
	case "", "none", "minimal", "low", "medium", "high", "xhigh", "max":
	default:
		return false
	}
	switch r.ReasoningSummary {
	case "", "auto", "concise", "detailed":
	default:
		return false
	}
	if r.Metadata != nil && !json.Valid(r.Metadata) {
		return false
	}
	if r.ProviderConfig != nil && (r.ProviderConfig.Wire != "anthropic" || !object(r.ProviderConfig.Thinking)) {
		return false
	}
	if format := r.OutputFormat; format != nil {
		if !validStrict(format.Strict) {
			return false
		}
		switch format.Kind {
		case "text", "json_object":
		case "json_schema":
			if !object(format.Schema) {
				return false
			}
		default:
			return false
		}
	}
	return true
}
func DecodeRequest(encoded []byte) (*Request, error) {
	var request Request
	if !utf8.Valid(encoded) || !Decode(encoded, &request) || !request.Valid() {
		return nil, fmt.Errorf("invalid canonical request")
	}
	return &request, nil
}

// PolicyView deliberately omits prompt bodies, schemas and opaque provider
// configuration. The policy edits only catalog paths; the host retains every
// original field. This keeps a large model transcript out of a bounded bus frame.
type PolicyMessage struct {
	Role string `json:"role"`
}
type PolicyTool struct {
	Kind       string       `json:"kind"`
	Name       string       `json:"name,omitempty"`
	Provider   string       `json:"provider,omitempty"`
	NativeType string       `json:"native_type,omitempty"`
	Children   []PolicyTool `json:"children,omitempty"`
}
type PolicyView struct {
	Version      int             `json:"version"`
	View         string          `json:"view"`
	Messages     []PolicyMessage `json:"messages"`
	Tools        []PolicyTool    `json:"tools"`
	LastUserText string          `json:"last_user_text"`
}

func (t PolicyTool) valid() bool {
	switch t.Kind {
	case "function":
		return t.Name != "" && len(t.Children) == 0 && t.Provider == "" && t.NativeType == ""
	case "hosted":
		return t.NativeType != "" && (t.Provider == "responses" || t.Provider == "anthropic" && t.Name != "") && len(t.Children) == 0
	case "namespace":
		if t.Name == "" || len(t.Children) > 4096 {
			return false
		}
		for _, child := range t.Children {
			if child.Kind != "function" || !child.valid() {
				return false
			}
		}
		return true
	default:
		return false
	}
}
func DecodePolicyView(encoded []byte) (*PolicyView, error) {
	var request PolicyView
	if !utf8.Valid(encoded) || !Decode(encoded, &request) || request.Version != 1 || request.View != "gateway_policy" ||
		request.Messages == nil || request.Tools == nil || len(request.Messages) > 4096 || len(request.Tools) > 4096 || len(request.LastUserText) > 65535 {
		return nil, fmt.Errorf("invalid canonical policy view")
	}
	for _, message := range request.Messages {
		if !validRole(message.Role) {
			return nil, fmt.Errorf("invalid canonical role")
		}
	}
	for _, tool := range request.Tools {
		if !tool.valid() {
			return nil, fmt.Errorf("invalid canonical tool")
		}
	}
	return &request, nil
}

// DecodeMessages admits canonical history independently of a provider request.
// Consumers may retain original JSON byte/order views after typed admission.
func DecodeMessages(encoded []byte) ([]Message, error) {
	var messages []Message
	if !utf8.Valid(encoded) || !Decode(encoded, &messages) || messages == nil || len(messages) > 4096 {
		return nil, fmt.Errorf("invalid canonical messages")
	}
	for _, message := range messages {
		if !validRole(message.Role) || message.Blocks == nil || len(message.Blocks) > 4096 {
			return nil, fmt.Errorf("invalid canonical message")
		}
		for _, block := range message.Blocks {
			if !validTypedBlock(block) || block.Type == "tool_use" && message.Role != "assistant" ||
				block.Type == "tool_result" && message.Role != "user" && message.Role != "tool" {
				return nil, fmt.Errorf("invalid canonical block")
			}
		}
	}
	return messages, nil
}
