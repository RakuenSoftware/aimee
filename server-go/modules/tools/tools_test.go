package tools

import (
	"encoding/binary"
	"strings"
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
		"bash":                    ClassExec,
		"execute_script":          ClassExec,
		"test":                    ClassExec,
		"run_tests":               ClassExec,
		"write_file":              ClassWrite,
		"git_pr":                  ClassRemote,
		"git_push":                ClassRemote,
		"web_search":              ClassRemote,
		"web_read":                ClassRemote,
		"git_verify":              ClassExec,
		"git_commit":              ClassWrite,
		"context_contract_expand": ClassControl,
		"request_input":           ClassControl,
		"diagnose_evidence":       ClassControl,
		"read_file":               ClassRead,
		"git_status":              ClassRead,
		"mcp:remote":              ClassRemote,
		"bash:remote":             ClassRemote,
		"not_registered":          ClassUnknown,
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

func metadataRequest(name string) []byte {
	request := make([]byte, 8+len(name))
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = metadataVersion
	binary.LittleEndian.PutUint16(request[6:8], uint16(len(name)))
	copy(request[8:], name)
	return request
}

func TestCompleteCatalogMetadataWire(t *testing.T) {
	if catalogError != nil {
		t.Fatal(catalogError)
	}
	if len(catalog) < 100 {
		t.Fatalf("incomplete catalog: %d tools", len(catalog))
	}
	for name, want := range catalog {
		request := metadataRequest(name)
		response, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, request)
		if status != bus.ModuleStatusOK || len(response) != metadataLen ||
			binary.LittleEndian.Uint32(response[0:4]) != metadataMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != want.Class ||
			binary.LittleEndian.Uint32(response[8:12]) != want.Primary ||
			binary.LittleEndian.Uint32(response[12:16]) != want.Categories {
			t.Fatalf("%s metadata response = %x (%v), want %+v", name, response, status, want)
		}
	}
	for _, name := range []string{"bash", "write_file", "read_file", "web_read", "request_input"} {
		if metadata(name).Class == ClassUnknown {
			t.Fatalf("missing %s", name)
		}
	}
	if metadata("new_unknown_tool").Categories != 0 {
		t.Fatal("unknown tool acquired policy metadata")
	}
	if got := metadata("remote:unknown"); got.Class != ClassRemote || got.Categories != PolicyExternal|PolicyNetwork {
		t.Fatalf("external tool metadata = %+v", got)
	}
}

func TestMetadataRefusesMalformedWire(t *testing.T) {
	for _, mutate := range []func([]byte){
		func(r []byte) { r[8] = 0 },
		func(r []byte) { r[6]++ },
		func(r []byte) { r[4] = 3 },
	} {
		request := metadataRequest("read_file")
		mutate(request)
		if _, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, request); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("malformed metadata wire accepted: %v", status)
		}
	}
	if _, err := loadCatalog([]byte(`{"read_file":{"class":"read","side_effect":"read","categories":[]}}`)); err == nil {
		t.Fatal("catalog accepted missing primary category")
	}
}

func TestMetadataPreservesLongQualifiedNamesAndRefusesBounds(t *testing.T) {
	name := "remote:" + strings.Repeat("x", 300)
	wire, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, metadataRequest(name))
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(wire[4:8]) != ClassRemote {
		t.Fatal("long external name refused", status)
	}
	for _, name := range []string{strings.Repeat("x", 4096), "remote:x\x00hidden"} {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, metadataRequest(name)); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("invalid name accepted", status)
		}
	}
	request := append(metadataRequest("read_file"), 0)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageDispatch}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("trailing bytes accepted")
	}
}
