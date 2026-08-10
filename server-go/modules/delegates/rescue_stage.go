package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	StageRescue uint32 = 6
	EventRescue uint32 = 6662

	rescueRequestMagic  uint32 = 0x51535244 /* "DRSQ" */
	rescueResponseMagic uint32 = 0x52535244 /* "DRSR" */
	rescueReqHeaderLen         = 16
	rescueRespHeaderLen        = 16
	rescueTextMax              = 1 << 20
	rescueKnownMax             = 4096

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
		request[4] != wireVersion || request[5] > 1 || request[6] > rescueModeDetect {
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
		out.known = append(out.known, string(request[at:at+n]))
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
func handleRescue(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	req, ok := decodeRescueRequest(request)
	if !ok {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	response := make([]byte, rescueRespHeaderLen)
	binary.LittleEndian.PutUint32(response[0:4], rescueResponseMagic)

	if req.mode == rescueModeDetect {
		if RescueHasToolCalls(req.text, req.known, req.allowJSON) {
			binary.LittleEndian.PutUint32(response[4:8], 1)
		}
		return response, bus.ModuleStatusOK
	}

	result, found := RescueParseToolCalls(req.text, req.known, req.allowJSON)
	if result.IsToolCall {
		binary.LittleEndian.PutUint32(response[4:8], 1)
	}
	binary.LittleEndian.PutUint32(response[8:12], uint32(found))
	binary.LittleEndian.PutUint32(response[12:16], uint32(len(result.Content)))
	response = append(response, result.Content...)
	for _, call := range result.Calls {
		var hdr [8]byte
		binary.LittleEndian.PutUint16(hdr[0:2], uint16(len(call.ID)))
		binary.LittleEndian.PutUint16(hdr[2:4], uint16(len(call.Name)))
		binary.LittleEndian.PutUint32(hdr[4:8], uint32(len(call.Arguments)))
		response = append(response, hdr[:]...)
		response = append(response, call.ID...)
		response = append(response, call.Name...)
		response = append(response, call.Arguments...)
	}
	return response, bus.ModuleStatusOK
}
