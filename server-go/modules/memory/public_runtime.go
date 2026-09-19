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
	case "fold_session":
		request.Operation, request.SessionID = "fold-session", args.stringOr("session_id", "")
		if strings.TrimSpace(request.SessionID) == "" {
			return invalid("missing session_id")
		}
		options.publicWrite = true
		scoped = commandScope(args, &request)
	case "anti_pattern_extract_from_feedback", "anti_pattern_extract_from_failures", "anti_pattern_escalate", "memory_learn_style", "scan_conversations":
		request.Operation = map[string]string{
			"anti_pattern_extract_from_feedback": "anti-pattern-feedback",
			"anti_pattern_extract_from_failures": "anti-pattern-failures",
			"anti_pattern_escalate":              "anti-pattern-escalate", "memory_learn_style": "learn-style",
			"scan_conversations": "scan-conversations",
		}[verb]
		request.HitThreshold = args.integer("hit_threshold", 5)
		if verb == "scan_conversations" {
			var dirs []json.RawMessage
			if raw, ok := args["dirs"]; !ok || string(raw) == "null" || json.Unmarshal(raw, &dirs) != nil {
				return invalid("missing dirs array")
			}
			for _, raw := range dirs {
				var dir string
				if json.Unmarshal(raw, &dir) == nil && dir != "" {
					request.Directories = append(request.Directories, dir)
				}
				if len(request.Directories) == 8 {
					break
				}
			}
		}
	case "episode_cards":
		request.Operation, request.SessionID, request.Limit = "episode-cards", args.stringOr("source_session", ""), args.limit("limit", 16, 64)
		if strings.TrimSpace(request.SessionID) == "" {
			return invalid("missing source_session")
		}
		scoped = commandScope(args, &request)
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
		if args.stringOr("view", "") == "server" {
			terms, limit, err := serverSearchArguments(args)
			if err != nil {
				return invalid(err.Error())
			}
			request.Operation, request.Query, request.Clusters = "server-search", strings.Join(terms, " "), terms
			request.Limit, request.PublicView = limit, true
			scoped = commandScope(args, &request)
			break
		}
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
	case "briefing", "alerts":
		format := args.stringOr("format", "")
		if format != "" && format != "text" && format != "json" && format != "mcp" {
			return invalid(verb + " format must be text, json or mcp")
		}
		if verb == "briefing" {
			request.Operation, request.LimitTokens = "briefing-bundle", args.integer("limit_tokens", 0)
			if request.LimitTokens < 0 {
				request.LimitTokens = 0
			}
			if request.LimitTokens > 8192 {
				request.LimitTokens = 8192
			}
		} else {
			request.Operation, request.AsOf = "alerts-bundle", args.stringOr("since", "")
			if request.AsOf != "" {
				if _, err := parseMemoryTime(request.AsOf); err != nil {
					return invalid(err.Error())
				}
			}
		}
		scoped = commandScope(args, &request)
		if format == "mcp" {
			scoped = true
			request.IncludeAll = args.boolean("include_all")
			request.Project, request.Workspace = args.stringOr("project", ""), args.stringOr("workspace", "")
			if request.Project == "__aimee_scope_missing__" {
				request.Project = ""
			}
			if request.Workspace == "__aimee_scope_missing__" {
				request.Workspace = ""
			}
		}
	case "context_block", "facts":
		var ok bool
		request.Query, ok = args.stringValue("query")
		if !ok {
			return invalid("missing query")
		}
		request.Operation, request.ContentCapacity = "fact-recall", 2048
		if verb == "context_block" {
			request.Operation = "context-ingress"
			request.BlockType, request.Limit = args.stringOr("block_type", "general"), args.limit("limit", 5, 100)
		}
		scoped = commandScope(args, &request)
	case "assemble_context":
		request.Operation, request.Query, request.Limit = "assemble-context", args.stringOr("task_hint", ""), 12
		if raw, exists := args["explain"]; exists && (string(raw) == "null" || json.Unmarshal(raw, &request.Detail) != nil) {
			return invalid("explain must be a boolean")
		}
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
	if verb == "facts" && request.Query == "" {
		return commandResult(map[string]any{"status": "ok", "facts": "", "active_context_missing": request.Workspace == "" && request.Project == ""})
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
	case "fold_session":
		if response.Count == nil {
			return nil, bus.ModuleStatusInternal
		}
		if *response.Count < 0 {
			result["status"], result["count"], result["message"] = "error", 0, "session fold was refused or incomplete"
		} else {
			result["count"] = *response.Count
		}
	case "episode_cards":
		if json.Unmarshal(response.Payload, &result) != nil {
			return nil, bus.ModuleStatusInternal
		}
		result["status"] = "ok"
	case "episode_card_generate":
		if response.Code != nil {
			if *response.Code == -3 {
				return commandResult(commandError("disabled", errEpisodeDisabled.Error()))
			}
			if *response.Code == -4 {
				return commandResult(commandError("capacity_exceeded", errEpisodeCapacity.Error()))
			}
			return commandResult(commandError("conflict", errEpisodeMixedScope.Error()))
		}
		if len(response.IDs) == 0 {
			return commandResult(commandError("not_found", "episode card generation produced no row"))
		}
		if len(response.IDs) != 1 || response.IDs[0] <= 0 {
			return nil, bus.ModuleStatusInternal
		}
		result["memory_unit_id"] = response.IDs[0]
	case "export_jsonl", "decisions_export_jsonl", "anti_pattern_extract_from_feedback", "anti_pattern_extract_from_failures", "anti_pattern_escalate", "memory_learn_style", "scan_conversations":
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
		if request.Operation == "server-search" {
			if response.PublicRecords == nil {
				response.PublicRecords = []publicMemoryRecord{}
			}
			result["store"], result["facts"], result["windows"] = "kb", response.PublicRecords, rows
		} else {
			result["results"] = rows
		}
	case "briefing", "alerts":
		if len(response.Payload) == 0 {
			return nil, bus.ModuleStatusInternal
		}
		if args.stringOr("format", "") != "" {
			output, err := memoryBundleOutput(verb, response.Payload, args, scoped && !request.IncludeAll && request.Project == "" && request.Workspace == "")
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			return commandResult(map[string]any{"status": "ok", "output": output})
		}
		result[verb] = response.Payload
	case "context_block", "facts":
		if response.Block == nil {
			return nil, bus.ModuleStatusInternal
		}
		field := "block"
		if verb == "facts" {
			field = "facts"
		}
		result[field] = *response.Block
		if response.Reason != "" {
			result["retraction"] = response.Reason
		}
	case "assemble_context":
		if response.Block == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["context"] = *response.Block
		if request.Detail {
			if response.ContextAssembly == nil {
				return nil, bus.ModuleStatusInternal
			}
			result["budget"] = response.ContextAssembly.Budget
			result["candidates"] = response.ContextAssembly.Candidates
			result["explain_text"] = response.ContextAssembly.ExplainText()
			result["candidate_scope"] = "returned_rows"
			result["token_estimator"] = "utf8_bytes_divided_by_four"
		}
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
		result["active_context_missing"] = !request.IncludeAll && request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}
