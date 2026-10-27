package governance

import (
	"encoding/binary"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/ir"
	"strings"
	"testing"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func policyWire(active bool, names []string, stopReason string) []byte {
	tools := make([]ir.ToolIdentity, len(names))
	for i, name := range names {
		tools[i] = ir.ToolIdentity{Name: name}
	}
	request, err := json.Marshal(policyRequest{Version: wireVersion, Active: &active, Tools: tools, StopReason: stopReason})
	if err != nil {
		panic(err)
	}
	return request
}

func decision(t *testing.T, response []byte) (uint32, uint32, string) {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion {
		t.Fatalf("invalid response %x", response)
	}
	reasonLen := int(binary.LittleEndian.Uint32(response[16:20]))
	return binary.LittleEndian.Uint32(response[8:12]),
		binary.LittleEndian.Uint32(response[12:16]),
		string(response[responseStopReasonOff : responseStopReasonOff+reasonLen])
}

func TestGovernanceDecisionParity(t *testing.T) {
	tests := []struct {
		name, stopReason, wantReason string
		active                       bool
		tools                        []string
		wantMask, wantDrops          uint32
	}{
		{"inactive preserves response", "unknown", "unknown", false, []string{"Agent", "read_file"}, 3, 0},
		{"empty response remains empty", "unknown", "unknown", true, nil, 0, 0},
		{"all denied ends turn", "tool_use", "end_turn", true,
			[]string{"spawn_agent", "Task"}, 0, 2},
		{"partial preserves explicit reason", "max_tokens", "max_tokens", true,
			[]string{"read_file", "RemoteTrigger", "write_file"}, 5, 1},
		{"partial preserves tool use", "tool_use", "tool_use", true,
			[]string{"Agent", "bash"}, 2, 1},
		{"none denied preserves reason", "content_filter", "content_filter", true,
			[]string{"agent", "delegate"}, 3, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(ring.ModuleInvocation{StageID: StageEvaluate},
				policyWire(test.active, test.tools, test.stopReason))
			if status != ring.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			mask, drops, reason := decision(t, response)
			if mask != test.wantMask || drops != test.wantDrops || reason != test.wantReason {
				t.Fatalf("decision = %#x/%d/%q, want %#x/%d/%q",
					mask, drops, reason, test.wantMask, test.wantDrops, test.wantReason)
			}
		})
	}
}

func TestGovernanceRejectsMalformedWire(t *testing.T) {
	valid := string(policyWire(true, []string{"Task"}, "tool_use"))
	tests := []string{
		"", "null", strings.Repeat("\x00", 632),
		strings.Replace(valid, `"version":2`, `"version":1`, 1),
		strings.Replace(valid, `"policy_active":true,`, ``, 1),
		strings.Replace(valid, `"policy_active":true`, `"policy_active":null`, 1),
		strings.Replace(valid, `"policy_active":true`, `"policy_active":1`, 1),
		strings.Replace(valid, `"Task"`, `""`, 1),
		strings.Replace(valid, `"Task"`, `"bad\u0000name"`, 1),
		strings.Replace(valid, `"Task"`, `"bad\ud800"`, 1),
		strings.Replace(valid, `"Task"`, `"Task","name":"allowed"`, 1),
		strings.Replace(valid, `"version":2`, `"version":2,"version":2`, 1),
		strings.Replace(valid, `"tools":[{"name":"Task"}]`, `"tools":null`, 1),
		strings.Replace(valid, `"tool_use"`, `"vendor-custom"`, 1),
		strings.Replace(valid, `"name":"Task"`, `"name":"Task","native":true`, 1),
		valid + ` {}`, valid + ` trailing`,
		string(policyWire(true, make([]string, 17), "tool_use")),
	}
	for i, request := range tests {
		if _, status := Handle(ring.ModuleInvocation{StageID: StageEvaluate}, []byte(request)); status != ring.ModuleStatusInvalidRequest {
			t.Errorf("malformed %d accepted: %s", i, request)
		}
	}
	if _, status := Handle(ring.ModuleInvocation{StageID: StageEvaluate + 1}, []byte(valid)); status != ring.ModuleStatusInvalidRequest {
		t.Fatal("wrong stage accepted")
	}
}

func TestGovernanceCompleteCanonicalIdentity(t *testing.T) {
	active := true
	name := "mcp__aimee__" + strings.Repeat("long_", 200) + "工具"
	namespace := "tenant/" + strings.Repeat("scope/", 100) + "😀"
	encoded, err := json.Marshal(policyRequest{Version: wireVersion, Active: &active, Tools: []ir.ToolIdentity{{Name: name, Namespace: namespace}, {Name: "Task", Namespace: namespace}}, StopReason: "tool_use"})
	if err != nil {
		t.Fatal(err)
	}
	decoded, ok := decodeRequest(encoded)
	if !ok || decoded.Tools[0].Name != name || decoded.Tools[0].Namespace != namespace || decoded.Tools[1].Namespace != namespace {
		t.Fatal("identity lost")
	}
	response, status := Handle(ring.ModuleInvocation{StageID: StageEvaluate}, encoded)
	if status != ring.ModuleStatusOK {
		t.Fatal(status)
	}
	mask, drops, reason := decision(t, response)
	if mask != 1 || drops != 1 || reason != "tool_use" {
		t.Fatalf("decision %d/%d/%s", mask, drops, reason)
	}
}

func TestGovernanceHonorsCancellationAfterValidation(t *testing.T) {
	invocation := ring.ModuleInvocation{StageID: StageEvaluate, DeadlineNS: 1}
	if _, status := Handle(invocation, policyWire(true, []string{"Task"}, "tool_use")); status != ring.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != ring.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
