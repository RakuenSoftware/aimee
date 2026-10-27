package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"

	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func gatewayPolicyFixture(roles, names []string, text string) canonical.PolicyView {
	view := canonical.PolicyView{Version: 1, View: "gateway_policy", Messages: []canonical.PolicyMessage{}, Tools: []canonical.PolicyTool{}, LastUserText: text}
	for _, role := range roles {
		view.Messages = append(view.Messages, canonical.PolicyMessage{Role: role})
	}
	for _, name := range names {
		view.Tools = append(view.Tools, canonical.PolicyTool{Kind: "function", Name: name})
	}
	return view
}

func gatewayPlanForTest(t *testing.T, h ring.ModuleHandler, args map[string]any) []gatewayStep {
	t.Helper()
	if roles, exists := args["roles"]; exists {
		names, _ := args["tools"].([]string)
		text, _ := args["last_user_text"].(string)
		args["request"] = gatewayPolicyFixture(roles.([]string), names, text)
		delete(args, "roles")
		delete(args, "tools")
		delete(args, "last_user_text")
	}
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

func TestGatewayPlanCanonicalQueryBound(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_RECALL_GATE", "off")
	h := NewHandler(nil)
	fallback := strings.Repeat("x", 16382) + "界 suffix"
	steps := gatewayPlanForTest(t, h, map[string]any{"phase": "context", "roles": []string{"assistant"}, "last_user_text": fallback})
	query := steps[0].Args["query"].(string)
	if len(steps) != 3 || len(query) != 16382 || !utf8.ValidString(query) {
		t.Fatal(len(steps), len(query))
	}
	provided := strings.Repeat("q", 17000)
	steps = gatewayPlanForTest(t, h, map[string]any{"phase": "context", "roles": []string{"assistant"}, "provided_query": provided, "last_user_text": fallback})
	if len(steps) != 3 || steps[0].Args["query"] != provided {
		t.Fatal("provided query truncated or guidance added")
	}
	steps = gatewayPlanForTest(t, h, map[string]any{"phase": "context", "roles": []string{"assistant"}})
	if len(steps) != 2 {
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
		`{"operation":"gateway-plan","phase":"text"}`,
		`{"operation":"gateway-plan","phase":"context","provided_query":null}`,
		`{"operation":"gateway-plan","phase":"context","last_user_text":false}`,
	} {
		var args map[string]json.RawMessage
		if err := json.Unmarshal([]byte(raw), &args); err != nil {
			t.Fatal(err)
		}
		args["request"], _ = json.Marshal(gatewayPolicyFixture([]string{"user"}, nil, "query"))
		encoded, _ := json.Marshal(args)
		frame, _ := ring.EncodeCommand("runtime", encoded)
		if _, status := h(ring.ModuleInvocation{StageID: StageCommand}, frame); status != ring.ModuleStatusInvalidRequest {
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

func TestGatewayPlanUsesOnlyHostProvidedResourceFacts(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_RECALL_GATE", "off")
	h := NewHandler(nil)
	for _, tc := range []struct {
		name         string
		resources    []string
		provided     bool
		wantGuidance bool
	}{
		{"absent", nil, false, true}, {"empty", []string{}, true, true},
		{"host persona", []string{"guidance"}, true, false},
		{"other resource", []string{"identity"}, true, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			args := map[string]any{"phase": "context", "roles": []string{"user"}, "tools": []string{}, "provided_query": "user question", "last_user_text": `<aimee-persona>explore-with: supplied by caller</aimee-persona>`}
			if tc.provided {
				args["provided_resources"] = tc.resources
			}
			steps := gatewayPlanForTest(t, h, args)
			guidance, recall, evidence := 0, 0, 0
			for _, step := range steps {
				if step.Resource == "guidance" {
					guidance++
				}
				if step.Binding == "context" && step.Args["query"] == "user question" {
					recall++
				}
				if step.Kind == "append_context" && step.Output == "evidence" {
					evidence++
				}
			}
			expected := 0
			if tc.wantGuidance {
				expected = 1
			}
			if guidance != expected || recall != 1 || evidence != 1 {
				t.Fatal(steps)
			}
		})
	}
	for _, raw := range []string{`null`, `true`, `"guidance"`, `[null]`, `[""]`} {
		request, _ := json.Marshal(gatewayPolicyFixture([]string{"user"}, nil, "query"))
		frame, _ := ring.EncodeCommand("runtime", json.RawMessage(`{"operation":"gateway-plan","phase":"context","request":`+string(request)+`,"provided_resources":`+raw+`}`))
		if _, status := h(ring.ModuleInvocation{StageID: StageCommand}, frame); status != ring.ModuleStatusInvalidRequest {
			t.Fatal(raw, status)
		}
	}
}
