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
	request := DataRequest{IncludeAll: true}
	switch operation {
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
