package economizer

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

// RecordBuildRequest is the session-compaction record seam. The caller already
// owns the transcript and range; the module owns coordinate extraction and the
// register grammar.
type RecordBuildRequest struct {
	Messages json.RawMessage `json:"messages"`
	Start    int             `json:"start"`
	End      int             `json:"end"`
}

type recordFields struct {
	Files     []string `json:"files_modified"`
	Errors    []string `json:"errors_encountered"`
	Decisions []string `json:"decisions_made"`
}

type recordBuildResponse struct {
	Record        recordFields `json:"record"`
	Closet        string       `json:"closet,omitempty"`
	ClosetEvicted bool         `json:"closet_evicted,omitempty"`
}

func recordMessageText(msg *JSONValue) string {
	content := msg.Get("content")
	if content == nil {
		return ""
	}
	if content.IsString() {
		return content.Str
	}
	if content.IsArray() {
		for _, block := range content.Items {
			if block.GetString("type") == "text" {
				return block.GetString("text")
			}
		}
	}
	return ""
}

func recordMessageIsToolResult(msg *JSONValue) bool {
	role := msg.GetString("role")
	if role == "" || role == "tool" {
		return true
	}
	if role != "user" {
		return false
	}
	content := msg.Get("content")
	if !content.IsArray() {
		return false
	}
	for _, block := range content.Items {
		if block.GetString("type") == "tool_result" {
			return true
		}
	}
	return false
}

// recordAddUnique deliberately checks the untrimmed value against prior stored
// values before applying the legacy byte cap, matching flashback_add_unique.
func recordAddUnique(items *[]string, value string, maxBytes int) {
	if value == "" {
		return
	}
	for _, item := range *items {
		if item == value {
			return
		}
	}
	if len(value) > maxBytes {
		value = value[:maxBytes]
	}
	*items = append(*items, value)
}

func buildRecord(messages *JSONValue, start, end int) recordBuildResponse {
	resp := recordBuildResponse{Record: recordFields{
		Files: []string{}, Errors: []string{}, Decisions: []string{},
	}}
	var set CoordSet
	rawTotal := 0
	for i := start; i < end; i++ {
		msg := messages.At(i)
		text := recordMessageText(msg)
		if text == "" {
			continue
		}
		role := msg.GetString("role")
		lane := LaneAgent
		if role == "user" && !recordMessageIsToolResult(msg) {
			lane = LaneUser
		}
		prov := Provenance{Lane: lane, TurnID: int64(i), ToolCallID: -1, ResultIndex: -1}
		NominateInto(text, &prov, &set)
		rawTotal += len(text)

		if role == "assistant" {
			switch ParseRegister(text) {
			case RegVerdict:
				recordAddUnique(&resp.Record.Decisions, text, 180)
			case RegHazard, RegBlocked:
				recordAddUnique(&resp.Record.Errors, text, 180)
			}
		}
	}
	for _, entry := range set.Items {
		if entry.Kind == CoordKindPath {
			recordAddUnique(&resp.Record.Files, entry.Value, 160)
		}
	}
	var evicted EvictResult
	resp.Closet, evicted = RenderCloset(&set, ClosetConfig{Enabled: true}, rawTotal)
	resp.ClosetEvicted = evicted == EvictFail
	return resp
}

func handleRecordBuild(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var req RecordBuildRequest
	if err := json.Unmarshal(request, &req); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	messages := ParseJSON(string(req.Messages))
	if messages == nil || !messages.IsArray() || req.Start < 0 || req.End < req.Start ||
		req.End > messages.Len() {
		return nil, bus.ModuleStatusInvalidRequest
	}
	resp := buildRecord(messages, req.Start, req.End)
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	body, err := json.Marshal(resp)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return body, bus.ModuleStatusOK
}
