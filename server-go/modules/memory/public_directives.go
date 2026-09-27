package memory

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleDirectiveCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: strings.ReplaceAll(verb, "_", "-"), IncludeAll: true}
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "directive_create":
		request.Question = args.stringOr("question", "")
		if request.Question == "" {
			return invalid("missing question")
		}
		request.Topic, request.AnchorEntity, request.AnchorFile = args.stringOr("topic", ""), args.stringOr("entity", ""), args.stringOr("file", "")
		request.Cause = args.stringOr("cause", "user_follow_up")
		if request.Cause == "" {
			request.Cause = "user_follow_up"
		}
		if !validDirectiveCause(request.Cause) {
			return invalid("memory directive create failed")
		}
		request.Priority = 50
		if n, ok := args.number("priority"); ok {
			request.Priority = int(math.Max(0, math.Min(100, n)))
		}
		request.SessionID, request.ValidUntil = args.stringOr("session", ""), args.stringOr("valid_until", "")
	case "directive_resolve", "directive_suppress":
		var ok bool
		request.ID, ok = args.positiveID("id")
		if !ok {
			return invalid("missing or invalid id")
		}
		if verb == "directive_resolve" {
			if n, ok := args.number("with_memory"); ok && n >= 0 && n <= 9007199254740991 {
				request.ResolutionMemoryID = int64(n)
			}
			// The public resolver has never overwritten evidence with the UI note.
			// Evidence updates remain an explicit data-owner operation.
		}
	case "directive_sweep_expired":
		request.Operation = "directive-sweep"
	case "directive_list":
		request.Limit = 50
		if n, ok := args.number("limit"); ok {
			request.Limit = int(math.Max(1, math.Min(256, n)))
		}
		request.State, request.Cause = args.stringOr("state", ""), args.stringOr("cause", "")
		if !validDirectiveState(request.State) || request.Cause != "" && !validDirectiveCause(request.Cause) {
			return commandResult(map[string]any{"status": "ok", "directives": []Directive{}})
		}
	case "directive_dashboard":
		request.Operation, request.State, request.Limit = "directive-list", "open", 20
	case "directive_briefing":
		request.Operation, request.State, request.Limit = "directive-current", "open", 5
		commandScope(args, &request)
		if n, ok := args.number("limit"); ok && n > 0 {
			request.Limit = int(math.Min(32, n))
		}
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
	case "directive_create", "directive_list":
		rows := make([]map[string]any, 0, len(response.Directives))
		for _, item := range response.Directives {
			encoded, _ := json.Marshal(item)
			var row map[string]any
			if json.Unmarshal(encoded, &row) != nil {
				return nil, bus.ModuleStatusInternal
			}
			delete(row, "source_session")
			delete(row, "updated_at")
			rows = append(rows, row)
		}
		if verb == "directive_list" {
			result["directives"] = rows
		} else {
			result["dedup"] = response.Deduplicated
			if !response.Deduplicated {
				if len(rows) != 1 {
					return nil, bus.ModuleStatusInternal
				}
				result["directive"] = rows[0]
			}
		}
	case "directive_resolve", "directive_suppress":
		if !response.Updated {
			return commandResult(commandError("not_found", "could not "+strings.TrimPrefix(verb, "directive_")+" directive (not open or missing)"))
		}
		result["id"] = request.ID
	case "directive_sweep_expired":
		result["expired"] = response.Expired
	case "directive_briefing":
		var block strings.Builder
		if len(response.Directives) > 0 {
			block.WriteString("# Open Questions\n")
			for _, item := range response.Directives {
				question := item.Question
				if question == "" {
					question = "(unnamed)"
				}
				fmt.Fprintf(&block, "- [p%d", item.Priority)
				if item.Cause != "" {
					fmt.Fprintf(&block, " · %s", item.Cause)
				}
				fmt.Fprintf(&block, "] %.300s\n", question)
			}
			block.WriteByte('\n')
		}
		result["block"] = block.String()
	case "directive_dashboard":
		data, status := handleData(options, invocation, []byte(`{"operation":"directive-count","include_all":true}`))
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		var counts DataResponse
		if json.Unmarshal(data, &counts) != nil || counts.DirectiveCounts == nil {
			return nil, bus.ModuleStatusInternal
		}
		c := counts.DirectiveCounts
		recent := make([]map[string]any, 0, len(response.Directives))
		for _, item := range response.Directives {
			recent = append(recent, map[string]any{"id": item.ID, "question": item.Question, "topic": item.Topic, "cause": item.Cause, "priority": item.Priority, "surfaced_count": item.SurfacedCount, "created_at": item.CreatedAt})
		}
		m := directiveMetrics()
		result["dashboard"] = map[string]any{"counts": map[string]int{"open": c.Open, "suppressed": c.Suppressed, "resolved": c.Resolved, "expired": c.Expired, "total": c.Open + c.Suppressed + c.Resolved + c.Expired}, "recent": recent,
			"metrics": map[string]any{"created_total": m.Created, "resolved_total": m.Resolved, "expired_total": m.Expired, "surfaced_total": m.Surfaced, "match_calls": m.Calls, "match_ms_avg": m.AverageMS, "match_ms_max": m.MaximumMS}}
	}
	return commandResult(result)
}
