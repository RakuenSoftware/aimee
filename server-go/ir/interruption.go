package ir

import (
	"encoding/json"
	"strconv"
	"strings"
)

// ToolInterruption is a host lifecycle observation, not an execution result.
// Schema admission grants neither source custody nor retry authorization.
type ToolInterruptionSource struct {
	Session    string `json:"session_id"`
	Turn       string `json:"turn_id"`
	Sequence   string `json:"sequence"`
	EventID    string `json:"event_id"`
	Actor      string `json:"actor"`
	MessageID  string `json:"message_id"`
	BlockIndex int    `json:"block_index"`
	ToolID     string `json:"tool_id"`
	Name       string `json:"name"`
}
type ToolInterruptionOwner struct {
	Owner       string `json:"owner_id"`
	Binding     string `json:"binding_sha256"`
	Epoch       string `json:"epoch"`
	Containment string `json:"containment_sha256"`
}
type ToolInterruption struct {
	Version  int                    `json:"version"`
	Source   ToolInterruptionSource `json:"source"`
	Owner    ToolInterruptionOwner  `json:"owner"`
	Terminal string                 `json:"terminal_sequence"`
	Outcome  string                 `json:"outcome"`
	Retry    string                 `json:"retry"`
}

const ToolInterruptionDomain = "native-interruption"
const ToolInterruptionText = "The host lost this tool's completion result. Its effects are unknown; interruption does not prove that it failed or was cancelled. Do not repeat an effect without explicit user authorization."

func ValidToolInterruption(raw []byte) bool {
	var r ToolInterruption
	if len(raw) > 8192 || !Decode(raw, &r) || r.Version != 1 || r.Outcome != "unknown" || r.Retry != "explicit_permission_required" {
		return false
	}
	positive := func(v string) bool {
		n, e := strconv.ParseUint(v, 10, 63)
		return e == nil && n > 0 && strconv.FormatUint(n, 10) == v
	}
	text := func(v string, max int) bool { return v != "" && len(v) <= max && !strings.ContainsRune(v, 0) }
	hex := func(v string, n int) bool { return len(v) == n && strings.Trim(v, "0123456789abcdef") == "" }
	var fields struct {
		Source map[string]json.RawMessage `json:"source"`
	}
	var index *int
	if json.Unmarshal(raw, &fields) != nil || !Decode(fields.Source["block_index"], &index) || index == nil {
		return false
	}
	s := r.Source
	return positive(s.Sequence) && positive(r.Terminal) && text(s.Session, 128) && text(s.Turn, 128) && text(s.EventID, 1024) && text(s.Actor, 128) && text(s.MessageID, 1024) && s.BlockIndex >= 0 && s.BlockIndex < 4096 && text(s.ToolID, 1024) && text(s.Name, 1024) && hex(r.Owner.Owner, 32) && hex(r.Owner.Binding, 64) && hex(r.Owner.Containment, 64) && (r.Owner.Epoch == "0" || hex(r.Owner.Epoch, 32))
}

// InterruptionMessage is a model-facing report that a result is unavailable,
// never a fabricated effect receipt. Its reserved context must be admitted only
// with the authenticated durable lifecycle sidecar by host native consumers.
func InterruptionMessage(r ToolInterruption, receiptID string) Message {
	text, _ := json.Marshal(ToolInterruptionText)
	yes := true
	return Message{Role: "tool", Blocks: []Block{{Type: "tool_result", ToolID: r.Source.ToolID, Content: text, IsError: &yes, Context: &Context{Origin: 1, Authority: 0, Trust: 4, Sensitivity: 1, ModelVisible: true, RevisionDomain: ToolInterruptionDomain, RevisionScope: receiptID}}}}
}

// InterruptionMessageJSON emits every C canonical context member, including
// diagnostic authority zero and revision epoch zero; the ordinary optional
// Context struct's omitempty tags are not a lossless cross-language encoder.
func InterruptionMessageJSON(r ToolInterruption, receiptID string) (json.RawMessage, error) {
	return json.Marshal(map[string]any{"role": "tool", "blocks": []any{map[string]any{"type": "tool_result", "tool_id": r.Source.ToolID, "content": ToolInterruptionText, "is_error": true, "context": map[string]any{"origin": 1, "authority": 0, "trust": 4, "sensitivity": 1, "model_visible": true, "revision_domain": ToolInterruptionDomain, "revision_scope": receiptID, "revision_epoch": "0"}}}})
}

// NativeExecutionOwner is host-only producer custody on an actual native
// message. It is not accepted from ordinary/provider ingress, even with a valid
// shape. Partial snapshots contain a closed contiguous prefix, never open calls.
type NativeExecutionOwner struct {
	Version int    `json:"version"`
	Harness string `json:"harness"`
	ToolInterruptionOwner
}

func ValidNativeExecutionOwner(raw []byte) bool {
	var owner NativeExecutionOwner
	var fields map[string]json.RawMessage
	if len(raw) > 2048 || !Decode(raw, &owner) || !Decode(raw, &fields) || len(fields) != 6 || owner.Version != 1 || (owner.Harness != "codex" && owner.Harness != "claude") {
		return false
	}
	hex := func(v string, n int) bool { return len(v) == n && strings.Trim(v, "0123456789abcdef") == "" }
	return hex(owner.Owner, 32) && hex(owner.Binding, 64) && hex(owner.Containment, 64) && (owner.Epoch == "0" || hex(owner.Epoch, 32))
}
