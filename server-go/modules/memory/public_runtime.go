package memory

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
)

func handleRuntimeCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "briefing":
		request.Operation, request.LimitTokens = "briefing-bundle", args.integer("limit_tokens", 0)
		if request.LimitTokens < 0 {
			request.LimitTokens = 0
		}
		if request.LimitTokens > 8192 {
			request.LimitTokens = 8192
		}
		scoped = commandScope(args, &request)
	case "alerts":
		request.Operation, request.AsOf = "alerts-bundle", args.stringOr("since", "")
		scoped = commandScope(args, &request)
	case "assemble_context":
		request.Operation, request.Query, request.Limit = "assemble-context", args.stringOr("task_hint", ""), 12
		scoped = commandScope(args, &request)
	case "compact_windows":
		request.Operation = "compact-legacy"
	case "query_edges":
		var ok bool
		request.Entity, ok = args.stringValue("entity")
		if !ok || request.Entity == "" {
			return invalid("missing entity")
		}
		request.Operation, request.Limit = "entity-edges", args.limit("max", 128, 256)
	case "check_drift":
		var ok bool
		request.ID, ok = args.positiveID("task_id")
		if !ok {
			return invalid("memory.check_drift requires task_id")
		}
		request.Operation, request.Path, request.Command = "check-drift", args.stringOr("file_path", ""), args.stringOr("command", "")
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory module unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok"}
	switch verb {
	case "briefing", "alerts":
		if len(response.Payload) == 0 {
			return nil, bus.ModuleStatusInternal
		}
		result[verb] = response.Payload
	case "assemble_context":
		if response.Block == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["context"] = *response.Block
	case "compact_windows":
		result["summaries"], result["facts"] = response.SummaryCount, response.FactCount
	case "query_edges":
		rows := make([]map[string]any, 0, len(response.Relations))
		for _, r := range response.Relations {
			rows = append(rows, map[string]any{"id": r.ID, "source": r.Source, "relation": r.Relation, "target": r.Target, "weight": r.Weight})
		}
		result["edges"] = rows
	case "check_drift":
		if response.Drift == nil {
			return commandResult(commandError("not_found", "task not found"))
		}
		r := response.Drift
		result["drifted"], result["task_id"], result["task_title"], result["message"] = r.Drifted, r.TaskID, r.TaskTitle, r.Message
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}
