package memory

import (
	"encoding/json"
	"errors"
	"fmt"

	"github.com/JBailes/aimee/server-go/bus"
)

type ingressBeginRequest struct {
	ContextLimits    *ContextLimits `json:"context_limits,omitempty"`
	Query            string         `json:"query"`
	Session          string         `json:"session"`
	Project          string         `json:"project"`
	ActiveScope      bool           `json:"active_scope"`
	Disabled         bool           `json:"disabled"`
	PreviewEnabled   bool           `json:"preview_enabled"`
	Mode             string         `json:"mode"`
	Budget           int            `json:"budget"`
	Compress         bool           `json:"compress"`
	CompressDisabled bool           `json:"compress_disabled"`
	CompressMin      int            `json:"compress_min"`
}

func ingressBegin(state *gatewayState, request ingressBeginRequest) map[string]any {
	result := map[string]any{"status": "ok", "active": false}
	if request.Disabled || request.Query == "" || !request.ActiveScope || request.Project == "" {
		return result
	}
	mode := request.Mode
	if mode != "off" && mode != "on" && mode != "observe" {
		if mode != "" {
			result["warning"] = fmt.Sprintf("invalid code_context_mode=%s; using observe", mode)
		}
		mode = "observe"
	}
	facts := mode != "on"
	if !request.PreviewEnabled && !facts {
		return result
	}
	budget := request.Budget
	if budget <= 0 {
		budget = 6144
	}
	budget, err := request.ContextLimits.byteLimit(budget)
	if err != nil {
		kind := "invalid_argument"
		var refusal *contextBudgetError
		if errors.As(err, &refusal) {
			kind = refusal.kind
		}
		result = commandError(kind, err.Error())
		result["active"] = false
		return result
	}
	// Do not consume a first-task claim or retrieve data for an unusable budget.
	if budget <= 384 {
		return result
	}
	if budget > maxDataBody {
		result["warning"] = "context byte limit exceeds memory message capacity"
		return result
	}
	result["active"], result["mode"] = true, mode
	result["legacy_preview"] = request.PreviewEnabled && mode != "on"
	result["facts"], result["temporal"] = facts, request.PreviewEnabled
	result["task"] = request.PreviewEnabled && mode != "off" && state.tasks.claim(request.Session, request.Project, request.Query)
	result["assembly"] = map[string]any{"operation": "ingress-assemble", "budget": budget,
		"context_limits": ContextLimits{SchemaVersion: 1, MaxContextBytes: &budget},
		"compress":       request.Compress && !request.CompressDisabled, "compress_min": request.CompressMin,
		"facts_requested": facts, "typed_requested": request.PreviewEnabled}
	return result
}

type ingressTaskResultRequest struct {
	Session     string          `json:"session"`
	Project     string          `json:"project"`
	Mode        string          `json:"mode"`
	HTTPStatus  int             `json:"http_status"`
	ElapsedMS   int64           `json:"elapsed_ms"`
	Unavailable bool            `json:"unavailable"`
	Packet      json.RawMessage `json:"packet"`
}

func ingressTaskResult(state *gatewayState, request ingressTaskResultRequest) map[string]any {
	if request.Unavailable {
		state.tasks.rearm(request.Session, request.Project)
	}
	block, count, confidence := "", 0, 0.0
	if request.HTTPStatus == 200 && request.ElapsedMS <= 2000 {
		block, count, confidence = ingressTaskContext(request.Packet, request.Project)
	}
	visible, effective, decision := 0, "observe", "suppressed"
	if block != "" {
		decision = "answerable"
		if request.Mode == "on" {
			visible, effective = 1, "on"
		}
	}
	log := fmt.Sprintf("mode=%s effective=%s project=%s status=%d latency_ms=%d decision=%s items=%d visible=%d",
		request.Mode, effective, request.Project, request.HTTPStatus, request.ElapsedMS, decision, count, visible)
	if visible == 0 {
		block = ""
	}
	return map[string]any{"status": "ok", "block": block, "confidence": confidence, "log": log}
}

func handleIngressPlan(state *gatewayState, args commandArgs) ([]byte, bus.ModuleStatus) {
	raw, err := json.Marshal(args)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	switch args.stringOr("operation", "") {
	case "ingress-begin":
		var request ingressBeginRequest
		_, limitsPresent := args["context_limits"]
		if json.Unmarshal(raw, &request) != nil || (limitsPresent && request.ContextLimits == nil) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		return commandResult(ingressBegin(state, request))
	case "ingress-task-result":
		var request ingressTaskResultRequest
		if json.Unmarshal(raw, &request) != nil || request.ElapsedMS < 0 || (request.Mode != "on" && request.Mode != "observe") {
			return nil, bus.ModuleStatusInvalidRequest
		}
		return commandResult(ingressTaskResult(state, request))
	case "ingress-recall-result":
		var unavailable *bool
		count, ok := args.number("count")
		if !ok || count < 0 || count > 5 || count != float64(int(count)) || json.Unmarshal(args["unavailable"], &unavailable) != nil || unavailable == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		result := map[string]any{"status": "ok"}
		if count == 0 && *unavailable {
			total := state.recallUnavailable.Add(1)
			project := args.stringOr("project", "")
			if project == "" {
				project = "-"
			}
			result["warning"] = fmt.Sprintf("memory recall UNAVAILABLE (not empty): the knowledge service did not answer; this turn's envelope carries no memory previews. project=%s total=%d", project, total)
		}
		return commandResult(result)
	case "ingress-metrics":
		return commandResult(map[string]any{"status": "ok", "recall_unavailable_total": state.recallUnavailable.Load()})
	}
	return nil, bus.ModuleStatusInvalidRequest
}
