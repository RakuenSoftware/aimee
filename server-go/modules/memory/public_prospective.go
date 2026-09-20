package memory

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Public prospective commands retain the KB envelopes. Request fields are
// selected explicitly; a public caller cannot inject a data operation or scope.
func handleProspectiveCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: strings.ReplaceAll(verb, "_", "-"), IncludeAll: true}
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	limit := func(key string, fallback, maximum int) int {
		n, ok := args.number(key)
		if !ok || n < 1 {
			return fallback
		}
		return int(math.Min(n, float64(maximum)))
	}
	switch verb {
	case "prospective_dashboard":
		request.Operation, request.Limit = "prospective-list", 20
	case "prospective_briefing":
		request.Operation, request.State, request.Limit = "prospective-current", "armed", limit("limit", 8, 32)
	case "prospective_list":
		request.State, request.Limit = strings.ToLower(strings.TrimSpace(args.stringOr("state", ""))), limit("limit", 50, 256)
		switch request.State {
		case "", "armed", "triggered", "completed", "expired":
		default:
			return invalid("invalid prospective state")
		}
	case "prospective_create":
		request.TriggerText, request.ActionText = args.stringOr("trigger_text", ""), args.stringOr("action_text", "")
		if request.TriggerText == "" {
			return invalid("missing trigger_text")
		}
		if request.ActionText == "" {
			return invalid("missing action_text")
		}
		request.AnchorEntity, request.AnchorFile = args.stringOr("anchor_entity", ""), args.stringOr("anchor_file", "")
		request.Recurrence, request.ValidUntil = strings.ToLower(strings.TrimSpace(args.stringOr("recurrence", "once"))), args.stringOr("valid_until", "")
		if request.Recurrence != "" && request.Recurrence != "once" && request.Recurrence != "repeat" {
			return invalid("create failed (check recurrence value)")
		}
	case "prospective_match":
		request.Query, request.AnchorEntity, request.AnchorFile = args.stringOr("turn_text", ""), args.stringOr("active_entity", ""), args.stringOr("active_file", "")
		request.Limit = limit("max", 3, 8)
	case "prospective_complete", "prospective_mark_triggered":
		var ok bool
		request.ID, ok = args.positiveID("id")
		if !ok {
			return invalid("missing or invalid id")
		}
	case "prospective_sweep_expired":
		request.Operation = "prospective-sweep"
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok"}
	switch verb {
	case "prospective_create", "prospective_list", "prospective_match", "prospective_dashboard", "prospective_briefing":
		rows := make([]map[string]any, 0, len(response.Prospectives))
		for _, item := range response.Prospectives {
			encoded, _ := json.Marshal(item)
			var row map[string]any
			if json.Unmarshal(encoded, &row) != nil {
				return nil, bus.ModuleStatusInternal
			}
			// The established public envelope does not expose the internal session.
			delete(row, "source_session")
			rows = append(rows, row)
		}
		switch verb {
		case "prospective_briefing":
			var block strings.Builder
			if len(response.Prospectives) > 0 {
				block.WriteString("# Open Commitments\n")
				for _, item := range response.Prospectives {
					trigger, action := item.TriggerText, item.ActionText
					if trigger == "" {
						trigger = "(no trigger)"
					}
					if action == "" {
						action = "(no action)"
					}
					fmt.Fprintf(&block, "- when `%.120s` → %.200s", trigger, action)
					if item.ValidUntil != "" {
						fmt.Fprintf(&block, "  [until %s]", item.ValidUntil)
					}
					block.WriteByte('\n')
				}
				block.WriteByte('\n')
			}
			result["block"] = block.String()
		case "prospective_dashboard":
			data, status := handleData(options, invocation, []byte(`{"operation":"prospective-count","include_all":true}`))
			if status != bus.ModuleStatusOK {
				return nil, status
			}
			var counts DataResponse
			if json.Unmarshal(data, &counts) != nil {
				return nil, bus.ModuleStatusInternal
			}
			for _, row := range rows {
				delete(row, "updated_at")
			}
			metrics := prospectiveMetrics()
			result["dashboard"] = map[string]any{"recent": rows,
				"counts": map[string]int{"armed": counts.Armed, "triggered": counts.Triggered, "completed": counts.Completed,
					"expired": counts.ProspectiveExpired, "total": counts.Armed + counts.Triggered + counts.Completed + counts.ProspectiveExpired},
				"metrics": map[string]any{"triggered_since_start": metrics.Triggered, "completed_since_start": metrics.Completed,
					"expired_since_start": metrics.Expired, "match_calls_since_start": metrics.Calls, "match_ms_avg": metrics.AverageMS, "match_ms_max": metrics.MaximumMS}}
		case "prospective_create":
			if len(rows) != 1 {
				return nil, bus.ModuleStatusInternal
			}
			result["prospective"] = rows[0]
		case "prospective_list":
			result["prospectives"] = rows
		case "prospective_match":
			result["matches"] = rows
		}
	case "prospective_complete":
		if !response.Updated {
			return commandResult(commandError("not_found", "could not complete (terminal or missing)"))
		}
	case "prospective_mark_triggered":
		if !response.Updated {
			return commandResult(commandError("not_found", "mark_triggered failed"))
		}
	case "prospective_sweep_expired":
		result["expired"] = response.ProspectiveExpired
	}
	return commandResult(result)
}
