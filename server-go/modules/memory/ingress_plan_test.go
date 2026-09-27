package memory

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestIngressPlanModesScopeAndOptOut(t *testing.T) {
	for _, scope := range []bool{false, true} {
		for _, preview := range []bool{false, true} {
			for _, mode := range []string{"on", "off", "observe", "", "invalid"} {
				state := &gatewayState{}
				request := ingressBeginRequest{Query: "fix local resolver", Session: "s", Project: "p", ActiveScope: scope,
					PreviewEnabled: preview, Mode: mode, Compress: true, CompressDisabled: true}
				plan := ingressBegin(state, request)
				active := scope && (preview || mode != "on")
				if plan["active"] != active {
					t.Fatal(request, plan)
				}
				if active {
					if plan["facts"] != (mode != "on") || plan["temporal"] != preview ||
						plan["legacy_preview"] != (preview && mode != "on") || plan["task"] != (preview && mode != "off") {
						t.Fatal(request, plan)
					}
					assembly := plan["assembly"].(map[string]any)
					if assembly["budget"] != 6144 || assembly["compress"] != false {
						t.Fatal(assembly)
					}
					limits := assembly["context_limits"].(ContextLimits)
					if limits.SchemaVersion != 1 || limits.MaxContextBytes == nil || *limits.MaxContextBytes != 6144 {
						t.Fatal("shipping host plan lacks versioned byte limit", limits)
					}
				}
				request.Disabled = true
				if plan := ingressBegin(state, request); plan["active"] != false {
					t.Fatal("request opt-out", plan)
				}
			}
		}
	}
	for _, request := range []ingressBeginRequest{
		{Query: "q", ActiveScope: true, PreviewEnabled: true},
		{Project: "p", ActiveScope: true, PreviewEnabled: true},
	} {
		if plan := ingressBegin(&gatewayState{}, request); plan["active"] != false {
			t.Fatal("missing query/project", plan)
		}
	}
}

func TestIngressPlanBudgetDoesNotConsumeTaskClaim(t *testing.T) {
	state := &gatewayState{}
	r := ingressBeginRequest{Query: "fix local resolver", Session: "s", Project: "p", ActiveScope: true, PreviewEnabled: true, Mode: "on", Budget: 384}
	if plan := ingressBegin(state, r); plan["active"] != false {
		t.Fatal(plan)
	}
	r.Budget = 1200
	if plan := ingressBegin(state, r); plan["task"] != true {
		t.Fatal("unused budget consumed a claim", plan)
	}
	if plan := ingressBegin(state, r); plan["task"] != false {
		t.Fatal("duplicate task", plan)
	}
}

func TestIngressRequirementsRefreshIndexForEveryAssembly(t *testing.T) {
	state := &gatewayState{}
	r := ingressBeginRequest{Query: "fix local resolver", Session: "s", Project: "p",
		ActiveScope: true, PreviewEnabled: true, Mode: "on", Budget: 1200}
	if ingressBegin(state, r)["task"] != true || ingressBegin(state, r)["task"] != false {
		t.Fatal("ordinary related turns must retain first-task suppression")
	}
	r.TaskRequirements = true
	for i := 0; i < 3; i++ {
		if ingressBegin(state, r)["task"] != true {
			t.Fatal("explicit obligations reused another assembly's index observation")
		}
	}
	r.Mode = "off"
	if ingressBegin(state, r)["task"] != false {
		t.Fatal("explicit obligations bypassed code-context opt-out")
	}
	r.Mode, r.Disabled = "on", true
	if ingressBegin(state, r)["active"] != false {
		t.Fatal("explicit obligations bypassed request opt-out")
	}
	r.Disabled, r.Budget = false, 384
	if ingressBegin(state, r)["active"] != false {
		t.Fatal("explicit obligations bypassed the usable-budget check")
	}
}

func TestIngressVersionedBudgetBeforeTaskClaim(t *testing.T) {
	state := &gatewayState{}
	zero := 0
	r := ingressBeginRequest{Query: "fix local resolver", Session: "s", Project: "p", ActiveScope: true,
		PreviewEnabled: true, Mode: "on", Budget: 1200, ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &zero}}
	if plan := ingressBegin(state, r); plan["active"] != false {
		t.Fatal("explicit zero inherited the legacy budget", plan)
	}
	r.ContextLimits = &ContextLimits{SchemaVersion: 1, MaxContextTokens: &zero}
	if plan := ingressBegin(state, r); plan["active"] != false || plan["kind"] != "unsupported_mode" {
		t.Fatal("unsupported token counting consumed retrieval work", plan)
	}
	r.ContextLimits = nil
	if plan := ingressBegin(state, r); plan["task"] != true {
		t.Fatal("refused limits consumed the first task claim", plan)
	}
}

func TestIngressTaskResultVisibilityLatencyAndRecovery(t *testing.T) {
	state := &gatewayState{}
	request := ingressTaskResultRequest{Session: "s", Project: "active-project", Mode: "on", HTTPStatus: 200, ElapsedMS: 2000, Packet: json.RawMessage(ingressPacketFixture)}
	result := ingressTaskResult(state, request)
	if result["block"] == "" || !strings.Contains(result["log"].(string), "visible=1") {
		t.Fatal(result)
	}
	request.ElapsedMS = 2001
	result = ingressTaskResult(state, request)
	if result["block"] != "" || !strings.Contains(result["log"].(string), "decision=suppressed items=0 visible=0") {
		t.Fatal(result)
	}
	request.ElapsedMS, request.Mode = 0, "observe"
	result = ingressTaskResult(state, request)
	if result["block"] != "" || !strings.Contains(result["log"].(string), "decision=answerable items=2 visible=0") {
		t.Fatal(result)
	}
	state.tasks.claim("s", "active-project", "fix local resolver")
	request.HTTPStatus = 503
	request.Mode = "on"
	result = ingressTaskResult(state, request)
	if result["block"] != "" || state.tasks.claim("s", "active-project", "fix local resolver") {
		t.Fatal("non-unavailable response rearmed the task", result)
	}
	request.Unavailable = true
	result = ingressTaskResult(state, request)
	if result["block"] != "" || !state.tasks.claim("s", "active-project", "fix local resolver") {
		t.Fatal("unavailable request did not rearm", result)
	}
}

func TestIngressPlanRuntimeAuthorityBothPlacements(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		plan := runHostRuntime(t, handler, `{"operation":"ingress-begin","project":"p","session":"s","query":"fix resolver","active_scope":true,"preview_enabled":true,"mode":"on"}`)
		if plan["task"] != true || plan["facts"] != false || plan["temporal"] != true || plan["legacy_preview"] != false {
			t.Fatal(plan)
		}
		for _, op := range []string{"ingress-begin", "ingress-task-result", "ingress-recall-result", "ingress-metrics"} {
			frame, _ := bus.EncodeCommand("runtime", []byte(fmt.Sprintf(`{"operation":%q}`, op)))
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(op, status)
			}
		}
	}
}

func TestIngressRecallUnavailableCounterIsAtomic(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	var wg sync.WaitGroup
	for i := 0; i < 64; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			outage := runHostRuntime(t, handler, `{"operation":"ingress-recall-result","count":0,"unavailable":true,"project":"p"}`)
			if !strings.Contains(outage["warning"].(string), "UNAVAILABLE (not empty)") {
				t.Error(outage)
			}
			quiet := runHostRuntime(t, handler, `{"operation":"ingress-recall-result","count":0,"unavailable":false}`)
			if quiet["warning"] != nil {
				t.Error(quiet)
			}
		}()
	}
	wg.Wait()
	metrics := runHostRuntime(t, handler, `{"operation":"ingress-metrics"}`)
	if metrics["recall_unavailable_total"] != float64(64) {
		t.Fatal(metrics)
	}
	for _, request := range []string{
		`{"operation":"ingress-task-result","mode":"off"}`,
		`{"operation":"ingress-task-result","mode":"on","elapsed_ms":-1}`,
		`{"operation":"ingress-recall-result","unavailable":true,"count":-1}`,
		`{"operation":"ingress-recall-result","unavailable":true,"count":0.5}`,
		`{"operation":"ingress-recall-result","unavailable":null,"count":0}`,
	} {
		frame, _ := bus.EncodeCommand("runtime", []byte(request))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(request, status)
		}
	}
}
