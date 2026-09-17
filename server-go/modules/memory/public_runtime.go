package memory

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"strings"
)

func handleRuntimeCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "episode_card_generate":
		request.Operation, request.SessionID = "episode-card-generate", args.stringOr("source_session", "")
		if strings.TrimSpace(request.SessionID) == "" {
			return invalid("missing source_session")
		}
		options.publicWrite = true
		scoped = commandScope(args, &request)
	case "export_jsonl", "decisions_export_jsonl":
		request.Operation = map[string]string{"export_jsonl": "export-jsonl", "decisions_export_jsonl": "export-decisions-jsonl"}[verb]
		request.Path = args.stringOr("path", "")
		if strings.TrimSpace(request.Path) == "" {
			return invalid("missing path")
		}
		scoped = commandScope(args, &request)
	case "search":
		request.Operation, request.Limit = "legacy-search", args.limit("limit", 10, 64)
		if raw, exists := args["clusters"]; exists {
			var clusters []json.RawMessage
			if json.Unmarshal(raw, &clusters) != nil {
				return invalid("clusters must be an array")
			}
			for _, raw := range clusters {
				var cluster string
				if json.Unmarshal(raw, &cluster) == nil && cluster != "" {
					request.Clusters = append(request.Clusters, cluster)
				}
				if len(request.Clusters) == 64 {
					break
				}
			}
		}
		scoped = commandScope(args, &request)
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
	case "episode_card_generate":
		if response.Code != nil {
			return commandResult(commandError("conflict", errEpisodeMixedScope.Error()))
		}
		if len(response.IDs) == 0 {
			return commandResult(commandError("not_found", "episode card generation produced no row"))
		}
		if len(response.IDs) != 1 || response.IDs[0] <= 0 {
			return nil, bus.ModuleStatusInternal
		}
		result["memory_unit_id"] = response.IDs[0]
	case "export_jsonl", "decisions_export_jsonl":
		if response.Count == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["count"] = *response.Count
	case "search":
		rows := response.LegacyResults
		if rows == nil {
			rows = []LegacySearchResult{}
		}
		for i := range rows {
			if rows[i].Files == nil {
				rows[i].Files = []string{}
			}
		}
		result["results"] = rows
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
