package ir

import (
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"unicode/utf8"
)

// Response is the owned aimee.response.v1 snapshot emitted by the C IR codec.
// Raw sidecars survive transport but never select content, completion or tools.
type Response struct {
	Schema        string          `json:"schema"`
	Role          *string         `json:"role"`
	Blocks        []ResponseBlock `json:"blocks"`
	Usage         ResponseUsage   `json:"usage"`
	StopReason    StopReason      `json:"stop_reason"`
	ID            *string         `json:"id,omitempty"`
	Model         *string         `json:"model,omitempty"`
	ErrorMessage  *string         `json:"error_message,omitempty"`
	RawStopReason *string         `json:"raw_stop_reason,omitempty"`
	Refusal       *RefusalDetails `json:"refusal,omitempty"`
	Raw           json.RawMessage `json:"raw,omitempty"`
}

// ResponseBlock adds the inert native sidecar supported by the C response codec.
// Session events deliberately keep their separate, sidecar-free Block contract.
type ResponseBlock struct {
	Block
	Raw json.RawMessage `json:"raw,omitempty"`
}

type ResponseUsage struct {
	Input      string `json:"input"`
	Output     string `json:"output"`
	CacheRead  string `json:"cache_read"`
	CacheWrite string `json:"cache_write"`
	Reasoning  string `json:"reasoning"`
}

type StopReason int

// Values are the existing aimee_stop_reason_t ABI, not provider finish strings.
const (
	StopUnknown StopReason = iota
	StopEndTurn
	StopMaxTokens
	StopToolUse
	StopSequence
	StopContentFilter
	StopError
)

func ZeroUsage() ResponseUsage { return ResponseUsage{"0", "0", "0", "0", "0"} }

func canonicalUnsigned(value string, bits int) bool {
	_, err := strconv.ParseUint(value, 10, bits)
	return err == nil && value != "" && (len(value) == 1 || value[0] != '0') && value[0] != '+'
}

func (c *Context) Valid() bool {
	if c == nil {
		return true
	}
	return c.Origin >= 0 && c.Origin <= 7 && c.Authority >= 0 && c.Authority <= 3 &&
		c.Trust >= 0 && c.Trust <= 4 && c.Sensitivity >= 0 && c.Sensitivity <= 3 &&
		len(c.RevisionDomain) < 48 && len(c.RevisionScope) < 128 &&
		!strings.ContainsRune(c.RevisionDomain+c.RevisionScope, 0) && utf8.ValidString(c.RevisionDomain+c.RevisionScope) &&
		(c.RevisionEpoch == "" || canonicalUnsigned(c.RevisionEpoch, 64))
}

func (r Response) Valid() bool {
	if r.Schema != "aimee.response.v1" || r.Role != nil && *r.Role != "assistant" ||
		r.Blocks == nil || len(r.Blocks) > 4096 || r.StopReason < StopUnknown || r.StopReason > StopError {
		return false
	}
	for _, value := range []string{r.Usage.Input, r.Usage.Output, r.Usage.CacheRead, r.Usage.CacheWrite, r.Usage.Reasoning} {
		// C usage is a nonnegative signed long on the shipped 64-bit platforms.
		if !canonicalUnsigned(value, 63) {
			return false
		}
	}
	for _, value := range []*string{r.ID, r.Model, r.ErrorMessage, r.RawStopReason} {
		if value != nil && (strings.ContainsRune(*value, 0) || !utf8.ValidString(*value)) {
			return false
		}
	}
	if r.Raw != nil && !ValidJSON(r.Raw) {
		return false
	}
	if r.Refusal != nil {
		for _, v := range []*string{r.Refusal.Category, r.Refusal.Explanation} {
			if v != nil && (!utf8.ValidString(*v) || strings.ContainsRune(*v, 0)) {
				return false
			}
		}
		raw, err := json.Marshal(r.Refusal)
		if r.StopReason != StopContentFilter || r.ErrorMessage != nil || err != nil || !validRefusalDetails(raw) {
			return false
		}
	}
	tools := make(map[string]bool)
	for _, block := range r.Blocks {
		if !validResponseStrings(block.Block) {
			return false
		}
		core, err := json.Marshal(block.Block)
		if err != nil || !ValidBlock(core) || !block.Context.Valid() ||
			block.Type == "tool_result" || block.Raw != nil && !ValidJSON(block.Raw) {
			return false
		}
		if block.Type == "tool_use" {
			if !object(block.Input) || tools[block.ToolID] {
				return false
			}
			tools[block.ToolID] = true
		}
	}
	return true
}

// DecodeResponse admits exactly the C snapshot schema, including its nullable
// implicit assistant role. No native provider JSON or display-string fallback.
func DecodeResponse(raw []byte) (*Response, error) {
	var fields map[string]json.RawMessage
	type wireResponse Response
	var wire wireResponse
	if !Decode(raw, &fields) || fields["role"] == nil || fields["blocks"] == nil ||
		fields["usage"] == nil || fields["stop_reason"] == nil ||
		!Decode(raw, &wire) || !Response(wire).Valid() {
		return nil, errors.New("invalid canonical response")
	}
	var stop StopReason
	if !Decode(fields["stop_reason"], &stop) {
		return nil, errors.New("invalid canonical stop reason")
	}
	for _, key := range []string{"id", "model", "error_message", "raw_stop_reason"} {
		if value := fields[key]; value != nil {
			var text string
			if !Decode(value, &text) {
				return nil, errors.New("invalid canonical optional string")
			}
		}
	}
	if details := fields["refusal"]; details != nil && string(details) != "null" && !validRefusalDetails(details) {
		return nil, errors.New("invalid canonical refusal shape")
	}
	var blocks []json.RawMessage
	if !Decode(fields["blocks"], &blocks) {
		return nil, errors.New("invalid canonical response blocks")
	}
	for _, rawBlock := range blocks {
		var fields map[string]json.RawMessage
		if !Decode(rawBlock, &fields) {
			return nil, errors.New("invalid canonical block")
		}
		delete(fields, "raw")
		core, _ := json.Marshal(fields)
		if !ValidBlock(core) {
			return nil, errors.New("invalid canonical block shape")
		}
	}
	response := Response(wire)
	return &response, nil
}

func (r *Response) UnmarshalJSON(raw []byte) error {
	parsed, err := DecodeResponse(raw)
	if err == nil {
		*r = *parsed
	}
	return err
}

// Clone transfers ownership without aliasing tool inputs, context or sidecars.
func (r Response) Clone() (*Response, error) {
	if !r.Valid() {
		return nil, errors.New("invalid canonical response")
	}
	raw, err := json.Marshal(r)
	if err != nil {
		return nil, err
	}
	return DecodeResponse(raw)
}

// DisplayText is a projection, never conversation history or execution status.
func (r Response) DisplayText() string {
	var out strings.Builder
	previous := 0
	for _, b := range r.Blocks {
		if b.Type != "text" || b.Text == nil || *b.Text == "" {
			continue
		}
		group := 0
		if b.MessageGroup != nil {
			group = *b.MessageGroup
		}
		if out.Len() > 0 && previous > 0 && group > 0 && previous != group {
			out.WriteString("\n\n")
		}
		out.WriteString(*b.Text)
		previous = group
	}
	return out.String()
}

// Complete reports a general admitted model completion. Media and opaque
// sidecars stay typed; this predicate grants no business text/JSON authority.
func (r Response) Complete() bool {
	if !r.Valid() || r.ErrorMessage != nil || r.Refusal != nil || (r.StopReason != StopEndTurn && r.StopReason != StopSequence) {
		return false
	}
	visible := false
	for _, block := range r.Blocks {
		switch block.Type {
		case "text":
			visible = visible || block.Text != nil && *block.Text != ""
		case "image", "document":
			visible = true
		case "thinking": // Accompanying opaque reasoning remains inert typed state.
		default:
			return false
		}
	}
	return visible
}

// CompleteText is the narrower canonical application-text predicate shared
// with C. SemanticText additionally refuses whitespace-only business answers.
func (r Response) CompleteText() bool {
	if !r.Complete() {
		return false
	}
	authored := false
	for _, block := range r.Blocks {
		if block.Type != "text" && block.Type != "thinking" {
			return false
		}
		authored = authored || block.Type == "text" && block.Text != nil && *block.Text != ""
	}
	return authored
}

// SemanticText is the explicit application-text/JSON boundary. A refusal,
// diagnostic, truncated/unknown finish or pending tool cannot become a verdict.
func (r Response) SemanticText() (string, error) {
	if !r.CompleteText() {
		return "", errors.New("canonical response is not a complete application text result")
	}
	text := r.DisplayText()
	if strings.TrimSpace(text) == "" {
		return "", errors.New("canonical response has no application text")
	}
	return text, nil
}

// TextResponse is an explicit declared text-transport/host composition boundary;
// structured provider adapters must preserve their typed content instead.
func TextResponse(text string) *Response {
	role := "assistant"
	return &Response{Schema: "aimee.response.v1", Role: &role,
		Blocks: []ResponseBlock{{Block: Block{Type: "text", Text: &text,
			Context: &Context{Origin: 7, Trust: 2, Sensitivity: 1, ModelVisible: true}}}},
		Usage: ZeroUsage(), StopReason: StopEndTurn}
}

// Context is complete on the C wire even when an enum/default value is zero.
func (b ResponseBlock) MarshalJSON() ([]byte, error) {
	type plain ResponseBlock
	raw, err := json.Marshal(plain(b))
	if err != nil {
		return nil, err
	}
	var fields map[string]json.RawMessage
	if err = json.Unmarshal(raw, &fields); err != nil {
		return nil, err
	}
	if b.Context != nil {
		c := b.Context
		epoch := c.RevisionEpoch
		if epoch == "" {
			epoch = "0"
		}
		fields["context"], err = json.Marshal(map[string]any{"origin": c.Origin, "authority": c.Authority, "trust": c.Trust, "sensitivity": c.Sensitivity, "model_visible": c.ModelVisible, "revision_domain": c.RevisionDomain, "revision_scope": c.RevisionScope, "revision_epoch": epoch})
		if err != nil {
			return nil, err
		}
	}
	return json.Marshal(fields)
}
func validResponseStrings(b Block) bool {
	values := []string{b.Type, b.ToolID, b.Name, b.Namespace, b.MediaType, b.MediaRef}
	for _, v := range []*string{b.Text, b.Signature, b.ThinkingBinding, b.RedactedContent, b.ThinkingID, b.EncryptedContent, b.CacheControl, b.CacheTTL} {
		if v != nil {
			values = append(values, *v)
		}
	}
	for _, v := range values {
		if !utf8.ValidString(v) || strings.ContainsRune(v, 0) {
			return false
		}
	}
	return true
}

// Context is a complete canonical object on the C wire, including zero enums
// and empty revisions. Omitting those fields makes C admission fail.
func (c Context) MarshalJSON() ([]byte, error) {
	if !(&c).Valid() {
		return nil, errors.New("invalid canonical context")
	}
	epoch := c.RevisionEpoch
	if epoch == "" {
		epoch = "0"
	}
	return json.Marshal(struct {
		Origin         int    `json:"origin"`
		Authority      int    `json:"authority"`
		Trust          int    `json:"trust"`
		Sensitivity    int    `json:"sensitivity"`
		ModelVisible   bool   `json:"model_visible"`
		RevisionDomain string `json:"revision_domain"`
		RevisionScope  string `json:"revision_scope"`
		RevisionEpoch  string `json:"revision_epoch"`
	}{c.Origin, c.Authority, c.Trust, c.Sensitivity, c.ModelVisible, c.RevisionDomain, c.RevisionScope, epoch})
}
func (c *Context) UnmarshalJSON(raw []byte) error {
	var fields map[string]json.RawMessage
	type plain Context
	var value plain
	if !Decode(raw, &fields) || len(fields) != 8 {
		return errors.New("invalid canonical context shape")
	}
	for _, key := range []string{"origin", "authority", "trust", "sensitivity", "model_visible", "revision_domain", "revision_scope", "revision_epoch"} {
		if fields[key] == nil || string(fields[key]) == "null" {
			return errors.New("missing canonical context field")
		}
	}
	if !Decode(raw, &value) || value.RevisionEpoch == "" || !(*Context)(&value).Valid() {
		return errors.New("invalid canonical context")
	}
	*c = Context(value)
	return nil
}
