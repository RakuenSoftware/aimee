package delegates

import (
	"encoding/binary"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/ir"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func rescueRequestBytes(text string, known []string, allowJSON bool, mode byte) []byte {
	request := make([]byte, rescueReqHeaderLen)
	binary.LittleEndian.PutUint32(request[0:4], rescueRequestMagic)
	request[4] = rescueWireVersion
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
	var responseIR rescueResponse
	if !ir.Decode(response, &responseIR) || responseIR.Version != 1 {
		t.Fatalf("bad canonical rescue envelope: %s", response)
	}
	out := RescueResult{IsToolCall: responseIR.Matched}
	if responseIR.Message != nil {
		encoded, _ := json.Marshal([]ir.Message{*responseIR.Message})
		if _, err := ir.DecodeMessages(encoded); err != nil {
			t.Fatal(err)
		}
		for _, block := range responseIR.Message.Blocks {
			if block.Type == "text" {
				out.Content = *block.Text
			}
			if block.Type == "tool_use" {
				out.Calls = append(out.Calls, RescueToolCall{ID: block.ToolID, Name: block.Name, Arguments: string(block.Input)})
			}
		}
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
	var envelope rescueResponse
	if !ir.Decode(response, &envelope) || envelope.Version != 1 || !envelope.Matched || envelope.Message != nil {
		t.Fatalf("detection invented model content: %s", response)
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

func TestRescueStageCanonicalLongCallsAndOpaqueArguments(t *testing.T) {
	name := strings.Repeat("long_name_", 30)
	arguments := `{"id":9007199254740993,"ratio":0.12345678901234567890123456789,"huge":1e9000,"unicode":"\ud83d\ude80"}`
	text := strings.Repeat("<tool_call><name>"+name+"</name><arguments>"+arguments+"</arguments></tool_call>", 40)
	response, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, rescueRequestBytes(text, []string{name}, false, rescueModeParse))
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	got := decodeRescueResponse(t, response)
	if len(got.Calls) != 40 {
		t.Fatalf("truncated call group: %d", len(got.Calls))
	}
	for _, call := range got.Calls {
		if call.Name != name || !strings.Contains(call.Arguments, "9007199254740993") || !strings.Contains(call.Arguments, "0.12345678901234567890123456789") || !strings.Contains(call.Arguments, "1e9000") {
			t.Fatal("name or opaque arguments changed", call)
		}
	}
}
func TestRescueStageRefusesMalformedArgumentsAndRetiredWire(t *testing.T) {
	for _, arguments := range []string{`[]`, `{"x":1} trailing`, `{"x":1,"x":2}`, `{"x":"\ud800"}`, `{"x":"\u0000"}`} {
		text := "<tool_call><name>bash</name><arguments>" + arguments + "</arguments></tool_call>"
		if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, rescueRequestBytes(text, []string{"bash"}, false, rescueModeParse)); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("accepted malformed arguments %s status=%d", arguments, status)
		}
	}
	retired := rescueRequestBytes("plain", nil, false, rescueModeParse)
	retired[4] = 1
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, retired); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("retired wire accepted")
	}
}
func TestRescueCallOverflowIsNotPartialDispatch(t *testing.T) {
	text := strings.Repeat("<tool_call><name>bash</name><arguments>{}</arguments></tool_call>", 4097)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRescue}, rescueRequestBytes(text, []string{"bash"}, false, rescueModeParse)); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("overflow became partial tool dispatch", status)
	}
}
func TestNativeArgumentValuesDoNotLoseTrailingText(t *testing.T) {
	var object orderedObject
	object.addParameterValue("value", "123suffix")
	if object.print() != `{"value":"123suffix"}` {
		t.Fatal(object.print())
	}
}
