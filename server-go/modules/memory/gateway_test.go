package memory

import (
	"encoding/json"
	"reflect"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestGatewayPlanSessionAndTools(t *testing.T) {
	h := NewHandler(nil)
	for _, roles := range [][]string{{}, {"system", "user", "user"}, {"user", "assistant", "user"}, {"assistant"}, {"Assistant", ""}} {
		args, _ := json.Marshal(map[string]any{"operation": "gateway-plan", "roles": roles, "tools": []string{"exec_command", "apply_patch", "local_shell", "shell", "bash", "run_command", "container.exec", "mcp__aimee__find_symbol", "Exec_command", "exec_command"}})
		r := runHostRuntime(t, h, string(args))
		wantStart := true
		for _, role := range roles {
			if role == "assistant" {
				wantStart = false
			}
		}
		wantRemove := []any{}
		if wantStart {
			wantRemove = []any{float64(9), float64(6), float64(5), float64(4), float64(3), float64(2), float64(0)}
		}
		if r["append_guidance"] != wantStart || !reflect.DeepEqual(r["remove_tools"], wantRemove) {
			t.Fatal(roles, r)
		}
	}
	for _, raw := range []string{`{"operation":"gateway-plan"}`, `{"operation":"gateway-plan","roles":null,"tools":[]}`, `{"operation":"gateway-plan","roles":[],"tools":[1]}`, `{"operation":"gateway-recall","query":null}`, `{"operation":"gateway-outcome","predicted_skip":true,"retrieval_needed":null}`} {
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(raw))
		if _, status := h(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(raw, status)
		}
	}
	for _, op := range []string{"gateway-plan", "gateway-recall", "gateway-outcome", "gateway-metrics", "gateway-enabled"} {
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(`{"operation":"`+op+`"}`))
		if _, status := h(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(op, status)
		}
	}
}

func TestGatewayEnableTokens(t *testing.T) {
	for _, value := range []string{"0", "off", "OFF", "False", "no", "NO"} {
		if gatewayEnabled(value) {
			t.Fatal(value)
		}
	}
	for _, value := range []string{"", "1", "on", "foo", "nope", "false ", " off", " false "} {
		if !gatewayEnabled(value) {
			t.Fatal(value)
		}
	}
	t.Setenv("AIMEE_STAGE_MEMORY", "OFF")
	if r := runHostRuntime(t, NewHandler(nil), `{"operation":"gateway-enabled","value":"on"}`); r["enabled"] != true {
		t.Fatal(r)
	}
	if r := runHostRuntime(t, NewHandler(nil), `{"operation":"gateway-enabled"}`); r["enabled"] != false {
		t.Fatal(r)
	}
}

func TestGatewayGateMetricsAndAudit(t *testing.T) {
	h := NewHandler(nil)
	for _, mode := range []string{"off", "observe", "enforce"} {
		t.Setenv("AIMEE_MEMORY_RECALL_GATE", mode)
		r := runHostRuntime(t, h, `{"operation":"gateway-recall","query":"thanks for that"}`)
		if r["skip"] != (mode != "off") || r["enforced"] != (mode == "enforce") {
			t.Fatal(mode, r)
		}
		if mode != "off" {
			if r["query_fingerprint"] != traceFingerprint("thanks for that") || strings.Contains(r["log"].(string), "thanks") {
				t.Fatal(r)
			}
			prefix := "RecallGateObservedSkip/"
			if mode == "enforce" {
				prefix = "RecallGateEnforcedSkip/"
			}
			if r["audit_role"] != prefix+"acknowledgement" {
				t.Fatal(r)
			}
		}
	}
	r := runHostRuntime(t, h, `{"operation":"gateway-recall","query":"where is config.go?"}`)
	if r["skip"] != false {
		t.Fatal(r)
	}
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			runHostRuntime(t, h, `{"operation":"gateway-outcome","predicted_skip":true,"retrieval_needed":true}`)
			runHostRuntime(t, h, `{"operation":"gateway-outcome","predicted_skip":false,"retrieval_needed":false}`)
		}()
	}
	wg.Wait()
	r = runHostRuntime(t, h, `{"operation":"gateway-metrics"}`)
	if r["predicted_skip"] != float64(2) || r["predicted_retrieve"] != float64(2) || r["wrongly_skipped"] != float64(20) || r["wrongly_performed"] != float64(20) {
		t.Fatal(r)
	}
	// A new handler is a new process lifetime, not a shared native/global counter.
	r = runHostRuntime(t, NewHandler(nil), `{"operation":"gateway-metrics"}`)
	if r["predicted_skip"] != float64(0) || r["wrongly_skipped"] != float64(0) {
		t.Fatal(r)
	}
}
