package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func rescueRequestBytes(text string, known []string, allowJSON bool, mode byte) []byte {
	request := make([]byte, rescueReqHeaderLen)
	binary.LittleEndian.PutUint32(request[0:4], rescueRequestMagic)
	request[4] = wireVersion
	if allowJSON {
		request[5] = 1
	}
	request[6] = mode
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(text)))
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(known)))
	request = append(request, text...)
	for _, name := range known {
		var n [2]byte
		binary.LittleEndian.PutUint16(n[:], uint16(len(name)))
		request = append(request, n[:]...)
		request = append(request, name...)
	}
	return request
}

func decodeRescueResponse(t *testing.T, response []byte) RescueResult {
	t.Helper()
	if len(response) < rescueRespHeaderLen ||
		binary.LittleEndian.Uint32(response[0:4]) != rescueResponseMagic {
		t.Fatalf("bad response envelope (%d bytes)", len(response))
	}
	out := RescueResult{IsToolCall: binary.LittleEndian.Uint32(response[4:8]) == 1}
	count := int(binary.LittleEndian.Uint32(response[8:12]))
	contentLen := int(binary.LittleEndian.Uint32(response[12:16]))
	at := rescueRespHeaderLen
	out.Content = string(response[at : at+contentLen])
	at += contentLen
	for i := 0; i < count; i++ {
		idLen := int(binary.LittleEndian.Uint16(response[at : at+2]))
		nameLen := int(binary.LittleEndian.Uint16(response[at+2 : at+4]))
		argsLen := int(binary.LittleEndian.Uint32(response[at+4 : at+8]))
		at += 8
		call := RescueToolCall{
			ID:        string(response[at : at+idLen]),
			Name:      string(response[at+idLen : at+idLen+nameLen]),
			Arguments: string(response[at+idLen+nameLen : at+idLen+nameLen+argsLen]),
		}
		at += idLen + nameLen + argsLen
		out.Calls = append(out.Calls, call)
	}
	if at != len(response) {
		t.Fatalf("response had %d trailing bytes", len(response)-at)
	}
	return out
}

func TestRescueStageRoundTripsACall(t *testing.T) {
	text := "Sure.<tool_call><name>bash</name><arguments>{\"command\":\"ls\"}</arguments></tool_call>"
	response, status := Handle(bus.ModuleInvocation{StageID: StageRescue},
		rescueRequestBytes(text, []string{"bash"}, true, rescueModeParse))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	got := decodeRescueResponse(t, response)
	if !got.IsToolCall || got.Content != "Sure." || len(got.Calls) != 1 {
		t.Fatalf("result = %+v", got)
	}
	if got.Calls[0].Name != "bash" || got.Calls[0].Arguments != `{"command":"ls"}` {
		t.Errorf("call = %+v", got.Calls[0])
	}
}

// The inventory travels in the request, so the same text is a call or not
// depending on what the caller said it accepts.
func TestRescueStageUsesTheCallersInventory(t *testing.T) {
	text := `{"name": "deploy", "arguments": {"env": "prod"}}`

	response, status := Handle(bus.ModuleInvocation{StageID: StageRescue},
		rescueRequestBytes(text, []string{"bash"}, true, rescueModeParse))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if got := decodeRescueResponse(t, response); len(got.Calls) != 0 {
		t.Errorf("unknown tool was rescued: %+v", got.Calls)
	}

	response, status = Handle(bus.ModuleInvocation{StageID: StageRescue},
		rescueRequestBytes(text, []string{"deploy"}, true, rescueModeParse))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if got := decodeRescueResponse(t, response); len(got.Calls) != 1 {
		t.Errorf("known tool was not rescued: %+v", got)
	}
}

func TestRescueStageDetectModeDoesNotParse(t *testing.T) {
	text := "<tool_call><name>bash</name><arguments>{}</arguments></tool_call>"
	response, status := Handle(bus.ModuleInvocation{StageID: StageRescue},
		rescueRequestBytes(text, []string{"bash"}, true, rescueModeDetect))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %d", status)
	}
	if len(response) != rescueRespHeaderLen {
		t.Fatalf("detect carried a body of %d bytes", len(response)-rescueRespHeaderLen)
	}
	if binary.LittleEndian.Uint32(response[4:8]) != 1 {
		t.Error("detect missed a tool call")
	}
}

func TestRescueStageRejectsInvalidEnvelope(t *testing.T) {
	good := rescueRequestBytes("hi", []string{"bash"}, true, rescueModeParse)

	short := good[:rescueReqHeaderLen-1]
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, short); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("truncated status = %d", status)
	}

	lying := rescueRequestBytes("hi", []string{"bash"}, true, rescueModeParse)
	binary.LittleEndian.PutUint32(lying[8:12], 9999)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, lying); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("length-mismatch status = %d", status)
	}

	// A name length that runs past the buffer must be refused, not read.
	overrun := rescueRequestBytes("hi", []string{"bash"}, true, rescueModeParse)
	binary.LittleEndian.PutUint16(overrun[rescueReqHeaderLen+2:rescueReqHeaderLen+4], 9999)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, overrun); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("overrunning name status = %d", status)
	}

	badMode := rescueRequestBytes("hi", []string{"bash"}, true, 7)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, badMode); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("bad-mode status = %d", status)
	}

	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue, DeadlineNS: 1},
		good); status != bus.ModuleStatusCancelled {
		t.Errorf("expired status = %d", status)
	}
}
