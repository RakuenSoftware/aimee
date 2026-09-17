package memory

import (
	"encoding/json"
	"math"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleMaintenanceCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	switch verb {
	case "lint":
		request.Operation, request.Limit = "lint", 256
	case "maintenance_run":
		request.Operation = "scheduled-maintenance"
		if n, ok := args.number("modes"); ok {
			if n < 0 || n > math.MaxUint32 || math.Trunc(n) != n {
				return commandResult(commandError("invalid_argument", "invalid maintenance modes"))
			}
			request.Modes = uint32(n)
		}
		_ = json.Unmarshal(args["force"], &request.Force)
		_ = json.Unmarshal(args["dry_run"], &request.DryRun)
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
	if verb == "maintenance_run" {
		if response.Maintenance == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["summary"] = response.Maintenance
	} else {
		issues := make([]map[string]any, 0, len(response.LintIssues))
		for _, item := range response.LintIssues {
			row := map[string]any{"type": item.Type, "key": item.Key, "message": item.Message}
			if item.MemoryID != 0 {
				row["memory_id"] = item.MemoryID
			}
			issues = append(issues, row)
		}
		result["issues"], result["issue_count"] = issues, len(issues)
	}
	return commandResult(result)
}
