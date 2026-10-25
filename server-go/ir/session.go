// Package ir defines the pure canonical wire contract shared by producers,
// storage and module consumers. It owns no state, I/O, policy or module calls.
package ir

import (
	"bytes"
	"encoding/json"
	"io"
	"strings"
	"unicode/utf8"
)

// Block retains typed structure and treats tool input/result JSON as opaque values.
type Block struct {
	Type             string          `json:"type"`
	Text             *string         `json:"text,omitempty"`
	Signature        *string         `json:"signature,omitempty"`
	ThinkingBinding  *string         `json:"thinking_binding,omitempty"`
	RedactedContent  *string         `json:"redacted_content,omitempty"`
	ThinkingID       *string         `json:"thinking_id,omitempty"`
	EncryptedContent *string         `json:"encrypted_content,omitempty"`
	CacheControl     *string         `json:"cache_control,omitempty"`
	CacheTTL         *string         `json:"cache_ttl,omitempty"`
	MessageGroup     *int            `json:"message_group,omitempty"`
	ToolID           string          `json:"tool_id,omitempty"`
	Name             string          `json:"name,omitempty"`
	Namespace        string          `json:"namespace,omitempty"`
	Input            json.RawMessage `json:"input,omitempty"`
	Content          json.RawMessage `json:"content,omitempty"`
	IsError          *bool           `json:"is_error,omitempty"`
	MediaType        string          `json:"media_type,omitempty"`
	MediaRef         string          `json:"media_ref,omitempty"`
	Context          *Context        `json:"context,omitempty"`
}
type Context struct {
	Origin         int    `json:"origin,omitempty"`
	Authority      int    `json:"authority,omitempty"`
	Trust          int    `json:"trust,omitempty"`
	Sensitivity    int    `json:"sensitivity,omitempty"`
	ModelVisible   bool   `json:"model_visible,omitempty"`
	RevisionDomain string `json:"revision_domain,omitempty"`
	RevisionScope  string `json:"revision_scope,omitempty"`
	RevisionEpoch  string `json:"revision_epoch,omitempty"`
}

// RefusalDetails is inert provider diagnostics, never model content or approval.
type RefusalDetails struct {
	Category    *string `json:"category"`
	Explanation *string `json:"explanation"`
}

func validRefusalDetails(raw json.RawMessage) bool {
	var fields map[string]json.RawMessage
	if !Decode(raw, &fields) || len(fields) != 2 || fields["category"] == nil || fields["explanation"] == nil {
		return false
	}
	var details RefusalDetails
	if !Decode(raw, &details) {
		return false
	}
	for _, field := range []struct {
		value *string
		limit int
	}{{details.Category, 64}, {details.Explanation, 4096}} {
		if field.value != nil && (len(*field.value) > field.limit || strings.ContainsRune(*field.value, 0)) {
			return false
		}
	}
	return true
}

type Message struct {
	ContentScalar bool    `json:"content_scalar,omitempty"`
	Role          string  `json:"role"`
	Blocks        []Block `json:"blocks"`
	Boundary      bool    `json:"_compaction_boundary,omitempty"`
}

// Validate the canonical contract, never a native provider protocol. Stateful
// checks (pending permissions, open blocks, ownership) remain the runtime's job;
// persisting a control response does not authorize its execution.
func ValidSessionEvent(encoded []byte) bool {
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
		NativeOwner json.RawMessage `json:"native_owner,omitempty"`
		Partial     json.RawMessage `json:"partial,omitempty"`
	}
	if !Decode(encoded, &e) {
		return false
	}
	if e.NativeOwner != nil && (e.Kind != "message" || !ValidNativeExecutionOwner(e.NativeOwner)) {
		return false
	}
	if e.Partial != nil {
		var partial bool
		if e.NativeOwner == nil || !Decode(e.Partial, &partial) || !partial {
			return false
		}
	}
	switch e.Kind {
	case "tool_interruption":
		return ValidToolInterruption(e.Payload)
	case "history":
		return ValidHistoryEvent(e.Payload)
	case "message":
		var m struct {
			Role   string            `json:"role"`
			Blocks []json.RawMessage `json:"blocks"`
		}
		if e.MessageID == "" || !Decode(e.Payload, &m) || m.Role == "" || m.Blocks == nil || len(m.Blocks) > 4096 {
			return false
		}
		if e.NativeOwner != nil && ((m.Role != "assistant" && m.Role != "tool") || (e.Role != "" && e.Role != m.Role) || (e.Partial != nil && (m.Role != "assistant" || len(m.Blocks) == 0))) {
			return false
		}
		for _, b := range m.Blocks {
			if !ValidBlock(b) {
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
			MessageGroup    json.RawMessage `json:"message_group"`
			ThinkingID      json.RawMessage `json:"thinking_id"`
			Encrypted       json.RawMessage `json:"encrypted_content"`
			Redacted        *string         `json:"redacted_content"`
			Namespace       string          `json:"namespace"`
			Input           json.RawMessage `json:"input"`
			StopReason      string          `json:"stop_reason"`
			RawStopReason   string          `json:"raw_stop_reason"`
			Refusal         json.RawMessage `json:"refusal"`
			UsageIn         *int64          `json:"usage_in"`
			UsageOut        *int64          `json:"usage_out"`
			UsageCacheRead  *int64          `json:"usage_cache_read"`
			UsageCacheWrite *int64          `json:"usage_cache_write"`
			UsageReasoning  *int64          `json:"usage_reasoning"`
			Error           string          `json:"error_message"`
		}
		if e.MessageID == "" || e.Role == "" || !Decode(e.Payload, &d) || (d.RawStopReason != "" && d.Type != "TURN_STOP") {
			return false
		}
		if d.Refusal != nil && (d.Type != "TURN_STOP" || (!bytes.Equal(bytes.TrimSpace(d.Refusal), []byte("null")) && (d.StopReason != "content_filter" || !validRefusalDetails(d.Refusal)))) {
			return false
		}
		if d.MessageGroup != nil {
			var group int
			if !Decode(d.MessageGroup, &group) || group < 1 || group > 4096 || d.Type != "BLOCK_START" || d.Kind != "text" {
				return false
			}
		}
		if (d.ThinkingID != nil || d.Encrypted != nil) &&
			(d.Kind != "thinking" || (d.Type != "BLOCK_START" && d.Type != "BLOCK_DELTA")) {
			return false
		}
		for _, value := range []json.RawMessage{d.ThinkingID, d.Encrypted} {
			if value != nil {
				var text string
				if !Decode(value, &text) {
					return false
				}
			}
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
				if d.Kind != "tool_use" || d.Type != "BLOCK_START" || !Decode(d.Input, &input) || input == nil {
					return false
				}
			}
			return (d.Text == nil || d.Kind == "text" || d.Kind == "thinking") && (d.Arguments == nil || (d.Kind == "tool_use" && d.Type == "BLOCK_DELTA"))
		default:
			return false
		}
	case "control_request", "control_response":
		var c map[string]json.RawMessage
		return e.RequestID != "" && e.ControlKind != "" && Decode(e.Payload, &c) && c != nil
	case "native_compaction":
		var compaction struct {
			Harness string `json:"harness"`
		}
		return Decode(e.Payload, &compaction) && (compaction.Harness == "codex" || compaction.Harness == "claude")
	case "native_checkpoint":
		var checkpoint struct {
			Scope    string `json:"scope"`
			Context  string `json:"context_digest"`
			Prefix   string `json:"prefix_digest"`
			Harness  string `json:"harness"`
			NativeID string `json:"native_id"`
			Claim    string `json:"claim"`
			Count    *int   `json:"message_count"`
		}
		if !Decode(e.Payload, &checkpoint) || checkpoint.Count == nil || *checkpoint.Count < 1 || *checkpoint.Count > 4096 ||
			(checkpoint.Harness != "codex" && checkpoint.Harness != "claude") || len(checkpoint.NativeID) == 0 || len(checkpoint.NativeID) > 1024 ||
			len(checkpoint.Claim) != 32 || strings.Trim(checkpoint.Claim, "0123456789abcdef") != "" {
			return false
		}
		for _, digest := range []string{checkpoint.Scope, checkpoint.Context, checkpoint.Prefix} {
			if len(digest) != 64 || strings.Trim(digest, "0123456789abcdef") != "" {
				return false
			}
		}
		return true
	case "replay_proof":
		var proof struct {
			Scope   string          `json:"scope"`
			Digest  string          `json:"block_digest"`
			Wire    string          `json:"wire"`
			Binding json.RawMessage `json:"thinking_binding,omitempty"`
		}
		if !Decode(e.Payload, &proof) || len(proof.Scope) != 64 || strings.Trim(proof.Scope, "0123456789abcdef") != "" ||
			len(proof.Digest) != 64 || strings.Trim(proof.Digest, "0123456789abcdef") != "" {
			return false
		}
		if proof.Wire == "responses" {
			return proof.Binding == nil
		}
		if proof.Wire != "anthropic" || proof.Binding == nil {
			return false
		}
		var value string
		if !Decode(proof.Binding, &value) {
			return false
		}
		return len(value) == 198 && strings.HasPrefix(value, "ap1:") && value[68] == ':' && value[133] == ':' &&
			strings.Trim(value[4:68]+value[69:133]+value[134:], "0123456789abcdef") == ""
	case "projection":
		var projection struct {
			Model           string          `json:"model"`
			Boundary        string          `json:"boundary"`
			Reasoning       *int64          `json:"omitted_reasoning_blocks"`
			Opaque          *int64          `json:"omitted_opaque_state_blocks"`
			CacheControls   int64           `json:"omitted_cache_controls,omitempty"`
			History         json.RawMessage `json:"history_messages_as_text,omitempty"`
			HostContext     json.RawMessage `json:"host_context_blocks_as_text,omitempty"`
			RequestControls json.RawMessage `json:"omitted_request_controls,omitempty"`
			EmptyMessages   json.RawMessage `json:"omitted_empty_messages,omitempty"`
			ExternalTools   json.RawMessage `json:"unverified_external_tool_reports,omitempty"`
		}
		if !Decode(e.Payload, &projection) {
			return false
		}
		counts := [5]int64{}
		for i, raw := range []json.RawMessage{projection.HostContext, projection.ExternalTools, projection.History, projection.RequestControls, projection.EmptyMessages} {
			if len(raw) != 0 && (bytes.Equal(bytes.TrimSpace(raw), []byte("null")) || !Decode(raw, &counts[i])) {
				return false
			}
		}
		return projection.Model != "" &&
			(projection.Boundary == "request" || projection.Boundary == "response") &&
			projection.Reasoning != nil && projection.Opaque != nil &&
			*projection.Reasoning >= 0 && *projection.Reasoning <= 9007199254740991 &&
			projection.CacheControls >= 0 && projection.CacheControls <= 9007199254740991 &&
			counts[0] >= 0 && counts[0] <= 4096 &&
			counts[1] >= 0 && counts[1] <= 4096 && counts[2] >= 0 && counts[2] <= 4096 && counts[3] >= 0 && counts[3] <= 4096 && counts[4] >= 0 && counts[4] <= 4096 &&
			(*projection.Reasoning > 0 || projection.CacheControls > 0 || counts[0] > 0 || counts[1] > 0 || counts[2] > 0 || counts[3] > 0 || counts[4] > 0) &&
			*projection.Opaque >= 0 && *projection.Opaque <= *projection.Reasoning
	case "child_execution":
		var child ChildExecutionPayload
		var fields map[string]json.RawMessage
		if !Decode(e.Payload, &fields) {
			return false
		}
		if binding, present := fields["learning_binding"]; present && bytes.Equal(bytes.TrimSpace(binding), []byte("null")) {
			return false
		}
		return Decode(e.Payload, &child) && ValidDelegateLearningBinding(child.LearningBinding) && child.Session != "" && len(child.Session) <= 127 &&
			child.Turn != "" && len(child.Turn) <= 127 && child.Role != "" && len(child.Role) <= 127 &&
			(child.Status == "running" || child.Status == "completed" || child.Status == "failed" || child.Status == "cancelled")
	case "execution":
		var x struct {
			Status             string          `json:"status"`
			Error              string          `json:"error,omitempty"`
			VerificationProofs json.RawMessage `json:"verification_proofs,omitempty"`
		}
		if !Decode(e.Payload, &x) {
			return false
		}
		if x.VerificationProofs != nil {
			var proofs []json.RawMessage
			if x.Status != "completed" || !Decode(x.VerificationProofs, &proofs) || len(proofs) < 1 || len(proofs) > 8 {
				return false
			}
			for _, proof := range proofs {
				if !ValidVerificationProof(proof) {
					return false
				}
			}
		}
		return x.Status == "running" || x.Status == "completed" || x.Status == "failed" || x.Status == "cancelled"
	default:
		return false
	}
}
func Decode(encoded []byte, out any) bool {
	if len(encoded) == 0 || bytes.Equal(bytes.TrimSpace(encoded), []byte("null")) {
		return false
	}
	if !ValidJSON(encoded) {
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
func ValidBlock(encoded []byte) bool {
	var b Block
	if !Decode(encoded, &b) || !irBlockKind(b.Type) || (b.MessageGroup != nil && (*b.MessageGroup < 1 || *b.MessageGroup > 4096)) {
		return false
	}
	var fields map[string]json.RawMessage
	if !Decode(encoded, &fields) {
		return false
	}
	allowed := map[string]bool{"type": true, "cache_control": true, "cache_ttl": true, "message_group": true, "context": true}
	specific := map[string][]string{"text": {"text"}, "thinking": {"text", "signature", "thinking_binding", "redacted_content", "thinking_id", "encrypted_content"}, "tool_use": {"tool_id", "name", "namespace", "input"}, "tool_result": {"tool_id", "content", "is_error"}, "image": {"media_type", "media_ref"}, "document": {"media_type", "media_ref"}}
	for _, key := range specific[b.Type] {
		allowed[key] = true
	}
	for key := range fields {
		if !allowed[key] {
			return false
		}
	}
	for _, key := range []string{"type", "text", "signature", "thinking_binding", "redacted_content", "thinking_id", "encrypted_content", "cache_control", "cache_ttl", "tool_id", "name", "namespace", "media_type", "media_ref"} {
		if value := fields[key]; value != nil {
			var text string
			if !Decode(value, &text) {
				return false
			}
		}
	}
	if raw := fields["context"]; raw != nil && bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return false
	}
	if raw := fields["message_group"]; raw != nil {
		var group int
		if !Decode(raw, &group) {
			return false
		}
	}
	if raw := fields["is_error"]; raw != nil {
		var value bool
		if !Decode(raw, &value) {
			return false
		}
	}

	if b.ThinkingBinding != nil {
		value := *b.ThinkingBinding
		if b.Type != "thinking" || len(value) != 198 || value[:4] != "ap1:" || (value[68] != ':' || value[133] != ':') {
			return false
		}
		for i := 4; i < len(value); i++ {
			if i != 68 && i != 133 && !(value[i] >= '0' && value[i] <= '9' || value[i] >= 'a' && value[i] <= 'f') {
				return false
			}
		}
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

// HistoryImportReport is host migration metadata, never model content/effect evidence.
type HistoryImportReport struct {
	Version                 int    `json:"version"`
	SourceFormat            string `json:"source_format"`
	Outcome                 string `json:"outcome"`
	SourceSHA256            string `json:"source_sha256"`
	SourceSession           string `json:"source_session"`
	SourceAgent             string `json:"source_agent"`
	SourceProvider          string `json:"source_provider"`
	SourceMessages          int    `json:"source_messages"`
	ImportedMessages        int    `json:"imported_messages"`
	RemovedThinkingBindings int    `json:"removed_thinking_bindings"`
	ToolStructure           string `json:"tool_structure"`
	ArchiveChunks           int    `json:"archive_chunks"`
}

func ValidHistoryImportReport(r *HistoryImportReport) bool {
	if r == nil || r.Version != 1 || len(r.SourceSHA256) != 64 || strings.Trim(r.SourceSHA256, "0123456789abcdef") != "" || r.SourceSession == "" || len(r.SourceSession) >= 128 || len(r.SourceAgent) >= 64 || len(r.SourceProvider) >= 64 || r.SourceMessages < 0 || r.SourceMessages > 16777216 || r.ImportedMessages < 0 || r.ImportedMessages > 16777216 || r.RemovedThinkingBindings < 0 || r.RemovedThinkingBindings > 16777216 || r.ArchiveChunks < 0 || r.ArchiveChunks > 4096 {
		return false
	}
	for _, v := range []string{r.SourceSession, r.SourceAgent, r.SourceProvider} {
		if strings.ContainsRune(v, 0) || !utf8.ValidString(v) {
			return false
		}
	}
	switch r.SourceFormat {
	case "canonical", "anthropic", "openai_chat", "responses", "tmux_text", "unknown":
	default:
		return false
	}
	switch r.Outcome {
	case "exact":
		return r.SourceFormat == "canonical" && r.SourceMessages == r.ImportedMessages && r.RemovedThinkingBindings == 0 && r.ToolStructure == "preserved" && r.ArchiveChunks == 0
	case "normalized":
		return r.SourceFormat != "tmux_text" && r.SourceFormat != "unknown" && r.ToolStructure == "preserved" && r.ArchiveChunks == 0
	case "archived":
		return r.SourceFormat == "tmux_text" && r.SourceMessages == 0 && r.ImportedMessages == 0 && r.RemovedThinkingBindings == 0 && r.ToolStructure == "unrecoverable" && r.ArchiveChunks > 0
	case "refused":
		return r.ImportedMessages == 0 && r.ToolStructure == "unrecoverable" && r.ArchiveChunks == 0
	}
	return false
}

func DecodeHistoryImportReport(raw json.RawMessage, r *HistoryImportReport) bool {
	var fields map[string]json.RawMessage
	if !Decode(raw, &fields) || len(fields) != 12 {
		return false
	}
	for _, v := range fields {
		if bytes.Equal(bytes.TrimSpace(v), []byte("null")) {
			return false
		}
	}
	return Decode(raw, r) && ValidHistoryImportReport(r)
}

type HistoryMessageFragment struct {
	MessageIndex  int    `json:"message_index"`
	PartIndex     int    `json:"part_index"`
	PartCount     int    `json:"part_count"`
	MessageSHA256 string `json:"message_sha256"`
	Text          string `json:"text"`
}

func ValidHistoryMessageFragment(f *HistoryMessageFragment) bool {
	return f != nil && f.MessageIndex >= 0 && f.MessageIndex < 16777216 && f.PartIndex >= 0 && f.PartIndex < 4096 && f.PartCount > 0 && f.PartCount <= 4096 && f.PartIndex < f.PartCount && len(f.MessageSHA256) == 64 && strings.Trim(f.MessageSHA256, "0123456789abcdef") == "" && len(f.Text) > 0 && len(f.Text) <= 65536 && utf8.ValidString(f.Text)
}

type HistoryEvent struct {
	Fragment     *HistoryMessageFragment `json:"fragment,omitempty"`
	ImportReport *HistoryImportReport    `json:"import_report,omitempty"`
	ArchiveText  *string                 `json:"archive_text,omitempty"`
	Scope        string                  `json:"scope"`
	Phase        string                  `json:"phase"`
	Index        int                     `json:"index,omitempty"`
	Count        int                     `json:"count,omitempty"`
	Message      json.RawMessage         `json:"message,omitempty"`
}

func ValidMessage(raw json.RawMessage) bool {
	var message struct {
		Role          string            `json:"role"`
		Blocks        []json.RawMessage `json:"blocks"`
		Boundary      bool              `json:"_compaction_boundary,omitempty"`
		ContentScalar *bool             `json:"content_scalar,omitempty"`
	}
	if !Decode(raw, &message) || message.Role == "" || message.Blocks == nil || len(message.Blocks) > 4096 {
		return false
	}
	for _, block := range message.Blocks {
		if !ValidBlock(block) {
			return false
		}
	}
	return true
}

func ValidHistoryEvent(raw json.RawMessage) bool {
	var event HistoryEvent
	if !Decode(raw, &event) || event.Scope == "" || len(event.Scope) > 128 || (event.Phase != "import_report" && event.ImportReport != nil) || (event.Phase != "archive" && event.ArchiveText != nil) || (event.Phase != "message_fragment" && event.Fragment != nil) {
		return false
	}
	switch event.Phase {
	case "message_fragment":
		if event.Index != 0 || event.Count != 0 || event.Message != nil || event.ImportReport != nil || event.ArchiveText != nil || !ValidHistoryMessageFragment(event.Fragment) {
			return false
		}
		var outer map[string]json.RawMessage
		var fields map[string]json.RawMessage
		if !Decode(raw, &outer) || len(outer) != 3 || !Decode(outer["fragment"], &fields) || len(fields) != 5 {
			return false
		}
		for _, v := range fields {
			if bytes.Equal(bytes.TrimSpace(v), []byte("null")) {
				return false
			}
		}
		return fields["message_index"] != nil && fields["part_index"] != nil && fields["part_count"] != nil && fields["message_sha256"] != nil && fields["text"] != nil
	case "import_report":
		var fields map[string]json.RawMessage
		var report HistoryImportReport
		if !Decode(raw, &fields) || len(fields) != 3 || !DecodeHistoryImportReport(fields["import_report"], &report) {
			return false
		}
		return event.Index == 0 && event.Count == 0 && event.Message == nil && event.ArchiveText == nil && ValidHistoryImportReport(event.ImportReport) && (event.ImportReport.Outcome != "archived" || event.Scope == "legacy-archive")
	case "archive":
		var fields map[string]json.RawMessage
		if !Decode(raw, &fields) || len(fields) != 4 || fields["index"] == nil || bytes.Equal(bytes.TrimSpace(fields["index"]), []byte("null")) {
			return false
		}
		return event.Scope == "legacy-archive" && event.Index >= 0 && event.Index < 4096 && event.Count == 0 && event.Message == nil && event.ImportReport == nil && event.ArchiveText != nil && len(*event.ArchiveText) > 0 && len(*event.ArchiveText) <= 65536 && utf8.ValidString(*event.ArchiveText)
	case "message":
		return event.Index >= 0 && event.Count == 0 && ValidMessage(event.Message)
	case "commit":
		return event.Count >= 0 && event.Index == 0 && event.Message == nil
	default:
		return false
	}
}

// Reject duplicate member names before typed decoding, including opaque values.
// Otherwise different producers and stores can disagree on the same JSON bytes.
func uniqueJSON(encoded []byte) bool {
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.UseNumber()
	var value func(int) bool
	value = func(depth int) bool {
		if depth > 512 {
			return false
		}
		token, err := decoder.Token()
		if err != nil {
			return false
		}
		delim, ok := token.(json.Delim)
		if !ok {
			return true
		}
		switch delim {
		case '{':
			names := make(map[string]bool)
			for decoder.More() {
				token, err := decoder.Token()
				name, ok := token.(string)
				if err != nil || !ok || names[name] {
					return false
				}
				names[name] = true
				if !value(depth + 1) {
					return false
				}
			}
			token, err = decoder.Token()
			return err == nil && token == json.Delim('}')
		case '[':
			for decoder.More() {
				if !value(depth + 1) {
					return false
				}
			}
			token, err = decoder.Token()
			return err == nil && token == json.Delim(']')
		default:
			return false
		}
	}
	if !value(0) {
		return false
	}
	_, err := decoder.Token()
	return err == io.EOF
}

// ValidJSON preserves opaque values while refusing spellings that native UTF-8
// strings cannot represent. In particular, replacement decoding must not turn
// an unpaired surrogate or embedded NUL into a different tool argument/key.
func ValidJSON(encoded []byte) bool {
	return utf8.Valid(encoded) && jsonScalarStrings(encoded) && uniqueJSON(encoded)
}
func jsonScalarStrings(encoded []byte) bool {
	hex := func(at int) (int, bool) {
		if at+4 > len(encoded) {
			return 0, false
		}
		n := 0
		for _, c := range encoded[at : at+4] {
			n *= 16
			switch {
			case c >= '0' && c <= '9':
				n += int(c - '0')
			case c >= 'a' && c <= 'f':
				n += int(c-'a') + 10
			case c >= 'A' && c <= 'F':
				n += int(c-'A') + 10
			default:
				return 0, false
			}
		}
		return n, true
	}
	quoted := false
	for i := 0; i < len(encoded); i++ {
		c := encoded[i]
		if c == '"' {
			quoted = !quoted
			continue
		}
		if !quoted {
			continue
		}
		if c < 0x20 {
			return false
		}
		if c != '\\' {
			continue
		}
		i++
		if i >= len(encoded) {
			return false
		}
		if encoded[i] != 'u' {
			continue
		}
		code, ok := hex(i + 1)
		if !ok || code == 0 {
			return false
		}
		i += 4
		if code >= 0xd800 && code <= 0xdbff {
			if i+6 >= len(encoded) || encoded[i+1] != '\\' || encoded[i+2] != 'u' {
				return false
			}
			low, ok := hex(i + 3)
			if !ok || low < 0xdc00 || low > 0xdfff {
				return false
			}
			i += 6
		} else if code >= 0xdc00 && code <= 0xdfff {
			return false
		}
	}
	return !quoted
}
