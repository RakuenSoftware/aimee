package memory

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"testing"
)

func TestRuntimeCommandHostOnly(t *testing.T) {
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		backend := &postgresDataStore{placement: placement, fusionEnabled: true}
		handler := NewHandler(nil, WithDataStore(placement, backend))
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"fusion-state","state":"off"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		state := runHostRuntime(t, handler, `{"operation":"fusion-state","state":"off"}`)
		if state["enabled"] != true || !backend.fusionEnabled {
			t.Fatal(state)
		}
		metrics := runHostRuntime(t, handler, `{"operation":"recall-metrics"}`)
		if _, ok := metrics["answer_counters"].(map[string]any); !ok {
			t.Fatal(metrics)
		}
		frame, _ = bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"scheduled-maintenance"}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if placement == PlacementServer {
			dashboard := runHostRuntime(t, handler, `{"operation":"maintenance-dashboard"}`)
			if dashboard["last"] != nil || dashboard["config"].(map[string]any)["interval_seconds"] != float64(900) {
				t.Fatal(dashboard)
			}
		}
	}
}

func runHostRuntime(t *testing.T, handler bus.ModuleHandler, args string) map[string]any {
	t.Helper()
	frame, err := bus.EncodeCommand("runtime", json.RawMessage(args))
	if err != nil {
		t.Fatal(err)
	}
	encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	body, err := bus.DecodeCommandResult(encoded)
	if err != nil {
		t.Fatal(err)
	}
	var result map[string]any
	if err := json.Unmarshal(body, &result); err != nil {
		t.Fatal(err)
	}
	return result
}
