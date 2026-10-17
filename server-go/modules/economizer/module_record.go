package economizer

import (
	"encoding/json"
	"strings"

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
	content := messageContent(msg)
	if content.IsString() {
		return content.Str
	}
	var text []string
	if content.IsArray() {
		for _, block := range content.Items {
			switch block.GetString("type") {
			case "text", "input_text", "output_text":
				text = append(text, block.GetString("text"))
			case "tool_result":
				result := block.Get("content")
				if result.IsString() {
					text = append(text, result.Str)
				}
				if result.IsArray() {
					for _, part := range result.Items {
						if part.GetString("type") == "text" {
							text = append(text, part.GetString("text"))
						}
					}
				}
			}
		}
	}
	return strings.Join(text, "\n")
}

func recordMessageIsToolResult(msg *JSONValue) bool {
	if msg.GetString("type") == "function_call_output" || msg.GetString("role") == "tool" {
		return true
	}
	content := messageContent(msg)
	if content.IsArray() {
		for _, block := range content.Items {
			if block.GetString("type") == "tool_result" {
				return true
			}
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
