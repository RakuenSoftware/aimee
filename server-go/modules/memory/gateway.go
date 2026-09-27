package memory

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"sync/atomic"

	"github.com/JBailes/aimee/server-go/bus"
)

// Gateway counters belong to one module handler/process, not to native callers.
// Evaluated outcomes stay separate: unnecessary recall cannot cancel missed recall.
type gatewayState struct {
	releases          sourceReleaseState
	tasks             ingressTaskState
	recallUnavailable atomic.Uint64
	predictedSkip     atomic.Uint64
	predictedRetrieve atomic.Uint64
	wronglySkipped    atomic.Uint64
	wronglyPerformed  atomic.Uint64
}

func (s *gatewayState) metrics() map[string]any {
	return map[string]any{"status": "ok", "predicted_skip": s.predictedSkip.Load(),
		"predicted_retrieve": s.predictedRetrieve.Load(), "wrongly_skipped": s.wronglySkipped.Load(),
		"wrongly_performed": s.wronglyPerformed.Load()}
}

func gatewayEnabled(value string) bool {
	for _, off := range []string{"0", "off", "false", "no"} {
		if strings.EqualFold(value, off) {
			return false
		}
	}
	return true
}

func handleGatewayCommand(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	switch args.stringOr("operation", "") {
	case "gateway-enabled":
		value := os.Getenv("AIMEE_STAGE_MEMORY")
		if raw, exists := args["value"]; exists {
			var supplied *string
			if json.Unmarshal(raw, &supplied) != nil || supplied == nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			value = *supplied
		}
		return commandResult(map[string]any{"status": "ok", "enabled": gatewayEnabled(value)})
	case "gateway-plan":
		return handleGatewayPlan(options, args)
	case "gateway-recall":
		var query *string
		if json.Unmarshal(args["query"], &query) != nil || query == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		return commandResult(gatewayRecall(options.gateway, *query))
	case "gateway-outcome":
		var predicted, needed *bool
		if json.Unmarshal(args["predicted_skip"], &predicted) != nil || json.Unmarshal(args["retrieval_needed"], &needed) != nil || predicted == nil || needed == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if *predicted && *needed {
			options.gateway.wronglySkipped.Add(1)
		} else if !*predicted && !*needed {
			options.gateway.wronglyPerformed.Add(1)
		}
		return commandResult(map[string]any{"status": "ok"})
	case "gateway-metrics":
		return commandResult(options.gateway.metrics())
	}
	return nil, bus.ModuleStatusInvalidRequest
}

func gatewayRecall(state *gatewayState, query string) map[string]any {
	enabled, enforce := recallGateMode()
	skip, reason := false, ""
	if enabled {
		skip, reason = recallGateDecision(query)
	}
	result := map[string]any{"status": "ok", "skip": skip, "enforced": skip && enforce}
	if skip {
		state.predictedSkip.Add(1)
		result["reason"] = reason
		mode, verb := "Observed", "would skip"
		if enforce {
			mode, verb = "Enforced", "skipping"
		}
		result["log"] = fmt.Sprintf("recall gate: %s turn (reason=%s)", verb, reason)
		result["audit_role"] = "RecallGate" + mode + "Skip/" + reason
		// The evidence API expects a fingerprint, never the raw user message.
		result["query_fingerprint"] = traceFingerprint(query)
	} else {
		state.predictedRetrieve.Add(1)
	}
	return result
}
