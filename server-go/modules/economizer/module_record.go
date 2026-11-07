package economizer

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
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

func recordBlockText(block ir.Block) string {
	switch block.Type {
	case "text":
		if block.Text != nil {
			return *block.Text
		}
	case "tool_result":
		var text string
		if json.Unmarshal(block.Content, &text) == nil {
			return text
		}
		if len(block.Content) > 0 {
			return string(block.Content)
		}
	}
	return ""
}
func recordMessageText(message ir.Message) string {
	var parts []string
	for _, block := range message.Blocks {
		if text := recordBlockText(block); text != "" {
			parts = append(parts, text)
		}
	}
	return strings.Join(parts, "\n")
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

func buildRecord(messages []ir.Message, start, end int) recordBuildResponse {
	resp := recordBuildResponse{Record: recordFields{
		Files: []string{}, Errors: []string{}, Decisions: []string{},
	}}
	var set CoordSet
	rawTotal := 0
	for i := start; i < end; i++ {
		message := messages[i]
		for _, block := range message.Blocks {
			text := recordBlockText(block)
			if text == "" {
				continue
			}
			lane := LaneAgent
			// A tool result in the same message cannot promote user text.
			if message.Role == "user" && block.Type != "tool_result" {
				lane = LaneUser
			}
			prov := Provenance{Lane: lane, TurnID: int64(i), ToolCallID: -1, ResultIndex: -1}
			NominateInto(text, &prov, &set)
			rawTotal += len(text)
		}
		if message.Role == "assistant" {
			text := recordMessageText(message)
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

func handleRecordBuild(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	var req RecordBuildRequest
	if !ir.Decode(request, &req) {
		return nil, ring.ModuleStatusInvalidRequest
	}
	messages, err := ir.DecodeMessages(req.Messages)
	if err != nil || req.Start < 0 || req.End < req.Start || req.End > len(messages) {
		return nil, ring.ModuleStatusInvalidRequest
	}
	resp := buildRecord(messages, req.Start, req.End)
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}
	body, err := json.Marshal(resp)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	return body, ring.ModuleStatusOK
}
