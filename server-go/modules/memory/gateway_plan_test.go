package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

func gatewayPlanForTest(t *testing.T, h bus.ModuleHandler, args map[string]any) []gatewayStep {
	t.Helper()
	args["operation"] = "gateway-plan"
	encoded, err := json.Marshal(args)
	if err != nil {
		t.Fatal(err)
	}
	reply := runHostRuntime(t, h, string(encoded))
	encoded, err = json.Marshal(reply["steps"])
	if err != nil {
		t.Fatal(err)
	}
	var steps []gatewayStep
	if err = json.Unmarshal(encoded, &steps); err != nil {
		t.Fatal(err)
	}
	return steps
}

func TestGatewayContextPlanAuthorityAndQuery(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_RECALL_GATE", "off")
	h := NewHandler(nil)
	steps := gatewayPlanForTest(t, h, map[string]any{"phase": "context", "roles": []string{"user"}, "tools": []string{}, "provided_query": "original question", "last_user_text": "persona text"})
	if len(steps) != 4 || steps[0].Binding != "context" || steps[0].Args["query"] != "original question" {
		t.Fatal(steps)
	}
	guidance, evidence := steps[1], steps[3]
	if guidance.Resource != "guidance" || guidance.Context["origin"] != "platform" || guidance.Context["authority"] != "task_instruction" || guidance.Context["trust"] != "verified" {
		t.Fatal(guidance)
	}
	if evidence.Output != "evidence" || evidence.WhenOutput != "evidence" || evidence.EpochOutput != "epoch" || evidence.Context["origin"] != "retrieval" || evidence.Context["authority"] != "evidence" || evidence.Context["trust"] != "unverified" {
		t.Fatal(evidence)
	}
	if steps[2].Binding != "epoch" || steps[2].WhenOutput != "evidence" {
		t.Fatal(steps[2])
	}
	// A later turn retains retrieval and never repeats opening guidance.
	steps = gatewayPlanForTest(t, h, map[string]any{"phase": "context", "roles": []string{"user", "assistant", "user"}, "tools": []string{}, "provided_query": "question"})
	if len(steps) != 3 || steps[0].Binding != "context" || steps[2].Output != "evidence" {
		t.Fatal(steps)
	}
}

func TestGatewayPlanQueryBoundAndTextMode(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_RECALL_GATE", "off")
	h := NewHandler(nil)
	fallback := strings.Repeat("x", 16382) + "界 suffix"
	steps := gatewayPlanForTest(t, h, map[string]any{"phase": "text", "last_user_text": fallback})
	query := steps[0].Args["query"].(string)
	if len(steps) != 1 || len(query) != 16382 || !utf8.ValidString(query) {
		t.Fatal(len(steps), len(query))
	}
	provided := strings.Repeat("q", 17000)
	steps = gatewayPlanForTest(t, h, map[string]any{"phase": "text", "provided_query": provided, "last_user_text": fallback})
	if len(steps) != 1 || steps[0].Args["query"] != provided {
		t.Fatal("provided query truncated or guidance added")
	}
	steps = gatewayPlanForTest(t, h, map[string]any{"phase": "text"})
	if len(steps) != 0 {
		t.Fatal(steps)
	}
}

func TestGatewayPlanEnforcedGateAndValidation(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_RECALL_GATE", "enforce")
	h := NewHandler(nil)
	steps := gatewayPlanForTest(t, h, map[string]any{"phase": "context", "roles": []string{}, "tools": []string{}, "provided_query": "thanks"})
	if len(steps) != 5 || steps[0].Binding != "log" || steps[1].Binding != "audit" || steps[1].Args["query_fingerprint"] != traceFingerprint("thanks") || steps[2].Resource != "guidance" {
		t.Fatal(steps)
	}
	for _, step := range steps {
		if step.Binding == "context" {
			t.Fatal("enforced skip invoked retrieval")
		}
	}
	for _, raw := range []string{
		`{"operation":"gateway-plan","phase":"unknown"}`,
		`{"operation":"gateway-plan","phase":null}`,
		`{"operation":"gateway-plan","phase":1}`,
		`{"operation":"gateway-plan","phase":"text","provided_query":null}`,
		`{"operation":"gateway-plan","phase":"text","last_user_text":false}`,
	} {
		frame, _ := bus.EncodeCommand("runtime", json.RawMessage(raw))
		if _, status := h(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(raw, status)
		}
	}
	// Planning tool removal or previewing a session must not count another recall.
	gatewayPlanForTest(t, h, map[string]any{"phase": "tools", "roles": []string{}, "tools": []string{"exec_command", "apply_patch", "shell"}})
	gatewayPlanForTest(t, h, map[string]any{"roles": []string{}, "tools": []string{}})
	metrics := runHostRuntime(t, h, `{"operation":"gateway-metrics"}`)
	if metrics["predicted_skip"] != float64(1) || metrics["predicted_retrieve"] != float64(0) {
		t.Fatal(metrics)
	}
}
