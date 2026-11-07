package delegates

import (
	"encoding/binary"
	"encoding/json"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const (
	StageRescue uint32 = 6
	EventRescue uint32 = 6662

	rescueRequestMagic uint32 = 0x51535244 /* "DRSQ" */
	rescueWireVersion  byte   = 2
	rescueReqHeaderLen        = 16
	rescueTextMax             = 1 << 20
	rescueKnownMax            = 4096

	// Detection runs on every model response, so it is its own mode rather
	// than a full parse whose result is thrown away.
	rescueModeParse  byte = 0
	rescueModeDetect byte = 1
)

type rescueRequest struct {
	mode      byte
	allowJSON bool
	text      string
	known     []string
}

func decodeRescueRequest(request []byte) (rescueRequest, bool) {
	var out rescueRequest
	if len(request) < rescueReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != rescueRequestMagic ||
		request[4] != rescueWireVersion || request[5] > 1 || request[6] > rescueModeDetect || request[7] != 0 {
		return out, false
	}
	out.allowJSON = request[5] == 1
	out.mode = request[6]

	textLen := int(binary.LittleEndian.Uint32(request[8:12]))
	knownCount := int(binary.LittleEndian.Uint32(request[12:16]))
	if textLen > rescueTextMax || knownCount > rescueKnownMax ||
		len(request) < rescueReqHeaderLen+textLen {
		return out, false
	}
	out.text = string(request[rescueReqHeaderLen : rescueReqHeaderLen+textLen])
	if !utf8.ValidString(out.text) || strings.ContainsRune(out.text, 0) {
		return out, false
	}

	out.known = make([]string, 0, knownCount)
	at := rescueReqHeaderLen + textLen
	for i := 0; i < knownCount; i++ {
		if at+2 > len(request) {
			return out, false
		}
		n := int(binary.LittleEndian.Uint16(request[at : at+2]))
		at += 2
		if at+n > len(request) {
			return out, false
		}
		name := string(request[at : at+n])
		if !utf8.ValidString(name) || strings.ContainsRune(name, 0) {
			return out, false
		}
		out.known = append(out.known, name)
		at += n
	}
	if at != len(request) {
		return out, false
	}
	return out, true
}

// handleRescue recovers tool calls a model wrote as text instead of making
// them properly.
//
// The request carries the caller's tool inventory because deciding whether a
// rescued name is real needs it and this module may not ask the tools module
// itself. The names are read and dropped; no caller state is kept here.
func handleRescue(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	req, ok := decodeRescueRequest(request)
	if !ok {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}

	response := rescueResponse{Version: 1}
	if req.mode == rescueModeDetect {
		response.Matched = RescueHasToolCalls(req.text, req.known, req.allowJSON)
	} else {
		result, found := RescueParseToolCalls(req.text, req.known, req.allowJSON)
		if found < 0 || found != len(result.Calls) {
			return nil, ring.ModuleStatusInvalidRequest
		}
		if found > 0 {
			message := ir.Message{Role: "assistant", Blocks: make([]ir.Block, 0, len(result.Calls)+1)}
			if result.Content != "" {
				text := result.Content
				message.Blocks = append(message.Blocks, ir.Block{Type: "text", Text: &text})
			}
			for _, call := range result.Calls {
				var input json.RawMessage
				if !strings.HasPrefix(strings.TrimSpace(call.Arguments), "{") || !ir.Decode([]byte(call.Arguments), &input) {
					return nil, ring.ModuleStatusInvalidRequest
				}
				message.Blocks = append(message.Blocks, ir.Block{Type: "tool_use", ToolID: call.ID, Name: call.Name, Input: input})
			}
			encoded, err := json.Marshal([]ir.Message{message})
			if err != nil {
				return nil, ring.ModuleStatusInternal
			}
			if _, err := ir.DecodeMessages(encoded); err != nil {
				return nil, ring.ModuleStatusInvalidRequest
			}
			response.Matched = true
			response.Message = &message
		}
	}
	encoded, err := json.Marshal(response)
	if err != nil {
		return nil, ring.ModuleStatusInternal
	}
	return encoded, ring.ModuleStatusOK
}

type rescueResponse struct {
	Version int         `json:"version"`
	Matched bool        `json:"matched"`
	Message *ir.Message `json:"message,omitempty"`
}
