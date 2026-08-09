package tools

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func toolRequest(name string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = wireVersion
	request[6] = byte(len(name))
	copy(request[8:], name)
	return request
}

func TestToolClassificationParity(t *testing.T) {
	tests := map[string]uint32{
		"bash":              ClassExec,
		"execute_script":    ClassExec,
		"test":              ClassExec,
		"run_tests":         ClassExec,
		"write_file":        ClassWrite,
		"git_pr":            ClassWrite,
		"request_input":     ClassControl,
		"diagnose_evidence": ClassControl,
		"read_file":         ClassRead,
		"git_status":        ClassRead,
		"mcp:remote":        ClassRemote,
		"bash:remote":       ClassRemote,
		"not_registered":    ClassUnknown,
	}
	for name, want := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, toolRequest(name))
		if status != bus.ModuleStatusOK || len(response) != responseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != want {
			t.Errorf("%q response = %x, status = %d, want class %d", name, response, status, want)
		}
	}
}

func TestToolsRejectInvalidWire(t *testing.T) {
	request := toolRequest("bash")
	request[7] = 1
	if _, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("reserved-byte status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageDispatch, DeadlineNS: 1},
		toolRequest("bash")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}
