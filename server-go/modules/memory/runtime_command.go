package memory

import (
	"context"
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// Runtime views are available only to the embedding host, never to a public
// RPC principal. All rendering and policy stays with the shared Go owner.
func handleRuntimeView(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	operation := args.stringOr("operation", "")
	switch operation {
	case "benchmark-hard-negative":
		return handleBenchmarkHardNegative(options, invocation, args)
	case "benchmark-miss":
		return handleBenchmarkMiss(options, invocation, args)
	case "benchmark-context":
		return handleBenchmarkContext(options, invocation, args)
	case "trace-patterns":
		return handleTracePatterns(args)
	case "workflow-plan", "workflow-result":
		return handleWorkflowObserver(operation, args)
	case "fusion-probe":
		return handleFusionProbe(options, invocation, args)
	case "code-context", "code-context-plan":
		return handleCodeContext(options, invocation, args)
	case "ingress-begin", "ingress-task-result", "ingress-recall-result", "ingress-metrics":
		return handleIngressPlan(options.gateway, args)
	case "ingress-assemble":
		return handleIngressAssembly(args)
	case "ingress-task-packet":
		return handleIngressTaskPacket(args)
	case "ingress-task-claim", "ingress-task-rearm", "ingress-task-reset":
		return handleIngressTaskState(&options.gateway.tasks, args)
	case "gateway-plan", "gateway-recall", "gateway-outcome", "gateway-metrics", "gateway-enabled":
		return handleGatewayCommand(options, invocation, args)
	case "user-store", "user-get", "user-list", "user-search", "user-delete", "user-supersede", "user-stats":
		return handleUserCommand(options, invocation, operation[len("user-"):], args)
	case "prospective-dashboard", "prospective-briefing", "directive-dashboard", "directive-briefing", "stats-dashboard":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		switch operation {
		case "prospective-dashboard":
			return handleProspectiveCommand(options, invocation, "prospective_dashboard", args)
		case "prospective-briefing":
			return handleProspectiveCommand(options, invocation, "prospective_briefing", args)
		case "directive-dashboard":
			return handleDirectiveCommand(options, invocation, "directive_dashboard", args)
		case "directive-briefing":
			return handleDirectiveCommand(options, invocation, "directive_briefing", args)
		default:
			return handleDomainCommand(options, invocation, "stats_dashboard", args)
		}
	}
	if operation == "learning-apply" {
		return handleLearningMutation(options, invocation, args)
	}
	if operation == "record" && options.placement == PlacementKB {
		return handleRecordCommand(options, invocation, "get", args)
	}
	request := DataRequest{IncludeAll: true}

	switch operation {
	case "trace-state", "trace-apply":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		request.Operation, request.SessionID = operation, args.stringOr("session_id", "")
		if operation == "trace-apply" && (json.Unmarshal(args["batch"], &request.TraceBatch) != nil || !validTraceBatch(request.TraceBatch)) {
			return nil, bus.ModuleStatusInvalidRequest
		}
	case "hybrid-context":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		request.Operation, request.Query, request.Entity = operation, args.stringOr("query", ""), args.stringOr("symbol", "")
		if request.Query == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
	case "convention-extract":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		request.Operation = operation
	case "demotion-run", "demotion-check":
		if options.placement != PlacementKB {
			return nil, bus.ModuleStatusCapabilityAbsent
		}
		if json.Unmarshal(args["config"], &request.Demotion) != nil || request.Demotion == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if operation == "demotion-run" && request.Demotion.Enabled == 0 {
			return commandResult(demotionSummary{Status: "ok"})
		}
		request.Operation = operation
	case "confidence":
		score, ok := args.number("score")
		if !ok {
			return nil, bus.ModuleStatusInvalidRequest
		}
		// Compare the fixed-point scale in Go. This preserves the legacy
		// threshold rounding and avoids undefined native float-to-int casts.
		band := confidenceForMicros(score * 1000000)
		name := map[uint32]string{ConfidenceLow: "low", ConfidenceMedium: "medium", ConfidenceHigh: "high"}[band]
		return commandResult(map[string]any{"status": "ok", "confidence": name})

	case "fact-review":
		caller := options.commandContext
		if caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" {
			return commandResult(commandError("unauthorized", "verified operator context required"))
		}
		request.Operation = operation
		var valid bool
		request.ID, valid = args.positiveID("id")
		request.State = args.stringOr("action", "")
		if !valid || (request.State != "approve" && request.State != "reject" && request.State != "undo") {
			return commandResult(commandError("invalid_argument", "positive assertion ID and review action required"))
		}
	case "feedback-path":
		request.Operation = operation
		request.Success = args.boolean("success")
		if json.Unmarshal(args["path"], &request.GraphPath) != nil || len(request.GraphPath) == 0 || len(request.GraphPath) > 32 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	case "record":
		request.Operation = "get"
		var valid bool
		request.ID, valid = args.positiveID("id")
		if !valid {
			return nil, bus.ModuleStatusInvalidRequest
		}
	case "directive-create":
		request.Operation = "directive-create"
		request.Question, request.Topic = args.stringOr("question", ""), args.stringOr("topic", "")
		request.Cause, request.AnchorEntity = args.stringOr("cause", "user_follow_up"), args.stringOr("entity", "")
		request.Priority = args.integer("priority", 50)
		request.Evidence, request.SessionID = args.stringOr("evidence", ""), args.stringOr("session", "")
	case "vector-search":
		request.Operation = operation
		request.RecordType = args.stringOr("record_type", "")
		request.MaxResults = args.limit("max_results", 16, 256)
		if json.Unmarshal(args["vector"], &request.Vector) != nil || len(request.Vector) == 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
	case "episode-list", "entity-profile", "fact-candidates":
		request.Operation, request.Query, request.Entity = operation, args.stringOr("query", ""), args.stringOr("entity", "")
		request.Limit = args.limit("limit", 16, 64)
		request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
	case "fusion-state":
		request.Operation = "fusion-state-get"
	case "recall-metrics":
		request.Operation = operation
	case "maintenance-dashboard":
		request.Operation = operation
	case "recall-dashboard":
		request.Operation, request.SessionStart = "recall-bundle", true
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	commandScope(args, &request)
	if operation == "trace-state" || operation == "trace-apply" {
		request.IncludeAll = args.boolean("include_all")
		request.Project, request.Workspace = args.stringOr("project", ""), args.stringOr("workspace", "")
		if raw, ok := args["scope"]; ok && json.Unmarshal(raw, &request.Scope) != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	body, _ := json.Marshal(request)
	encoded, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(encoded, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	switch operation {
	case "hybrid-context", "trace-state", "trace-apply":
		return commandResult(response.Payload)
	case "convention-extract":
		if response.Count == nil {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "emitted": *response.Count})
	case "fact-review", "fact-candidates", "demotion-run", "demotion-check":
		return commandResult(response.Payload)
	case "feedback-path":
		return commandResult(map[string]any{"status": "ok", "updated": response.Updated})
	case "record":
		if len(response.Records) == 0 {
			return commandResult(commandError("not_found", "memory not found"))
		}
		if len(response.Records) != 1 {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "memory": response.Records[0]})
	case "directive-create":
		if len(response.Directives) != 1 {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "directive": response.Directives[0], "dedup": response.Deduplicated})
	case "vector-search":
		if response.VectorHits == nil {
			response.VectorHits = []VectorHit{}
		}
		return commandResult(map[string]any{"hits": response.VectorHits})
	case "episode-list":
		if response.Episodes == nil {
			response.Episodes = []Episode{}
		}
		return commandResult(map[string]any{"status": "ok", "episodes": response.Episodes})
	case "entity-profile":
		if response.EntityProfile == nil {
			return commandResult(commandError("not_found", "entity profile not found"))
		}
		return commandResult(map[string]any{"status": "ok", "profile": response.EntityProfile})
	case "fusion-state":
		if response.Allowed == nil {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"enabled": *response.Allowed})
	case "recall-metrics":
		return commandResult(response.Metrics)
	case "maintenance-dashboard":
		return commandResult(response.Payload)
	case "recall-dashboard":
		var bundle map[string]any
		if json.Unmarshal(response.Payload, &bundle) != nil || bundle == nil {
			return nil, bus.ModuleStatusInternal
		}
		m := recallMetrics()
		bundle["metrics"] = map[string]any{"assemblies_total": m.Assemblies, "session_start_assemblies": m.Starts, "ms_avg": m.AverageMS, "ms_max": m.MaximumMS, "answer_counters": m.AnswerCounters}
		return commandResult(bundle)
	}
	return nil, bus.ModuleStatusInvalidRequest
}

func (s *postgresDataStore) maintenanceDashboard(ctx context.Context) (json.RawMessage, error) {
	var last *MaintenanceSummary
	if s.placement == PlacementKB {
		var raw string
		err := s.db.QueryRow(ctx, `SELECT value FROM kb_meta WHERE key='memory_maintenance_last_summary'`).Scan(&raw)
		if err != nil && !store.IsNoRows(err) {
			return nil, err
		}
		if err == nil {
			if err := json.Unmarshal([]byte(raw), &last); err != nil {
				return nil, err
			}
		}
	}
	values := map[string]any{}
	if s.settings != nil {
		var err error
		values, err = s.settings()
		if err != nil {
			return nil, err
		}
	}
	number := func(key string) float64 {
		switch value := values[key].(type) {
		case float64:
			return value
		case int:
			return float64(value)
		case bool:
			if value {
				return 1
			}
		case json.Number:
			n, _ := value.Float64()
			return n
		}
		return 0
	}
	interval := number("memory_maintenance_interval_seconds")
	if interval <= 0 {
		interval = 900
	}
	runs, avg, max := runtimeMetricState.maintenanceCalls.snapshot()
	return json.Marshal(map[string]any{
		"last":    last,
		"metrics": map[string]any{"runs_total": runs, "skips_total": runtimeMetricState.maintenanceSkips.Load(), "changes_total": runtimeMetricState.maintenanceChanges.Load(), "ms_avg": avg, "ms_max": max},
		"config":  map[string]any{"enabled": number("memory_maintenance_enabled") != 0, "interval_seconds": interval, "summarize_enabled": number("memory_maintenance_summarize_enabled") != 0},
	})
}
