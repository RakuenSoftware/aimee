package memory

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"

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
		} else if args.stringOr("view", "") == "console" {
			request.Modes = parseMaintenanceModes(args.stringOr("modes_csv", ""))
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
		switch args.stringOr("view", "") {
		case "console":
			addMaintenanceConsole(result, *response.Maintenance, request)
		case "model":
			summary, err := json.Marshal(response.Maintenance)
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			text := string(summary)
			var droppedPrune bool
			_ = json.Unmarshal(args["prune_removed"], &droppedPrune)
			// The transport's notice must agree with the owner's actual modes.
			if droppedPrune && response.Maintenance.ModesRun != 0 && response.Maintenance.ModesRun&MaintenancePrune == 0 {
				outcome := "The other requested modes ran."
				if response.Maintenance.Skipped {
					outcome = "The remaining modes were skipped by the idle guard."
				} else if request.DryRun {
					outcome = "The other requested modes were evaluated as a dry run."
				}
				text += "\n(prune was NOT run: it permanently deletes memories and is an operator action, `aimee memory maintain`. " + outcome + ")"
			}
			result["text"] = text
		}
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

// Preserve the CLI's comma/ASCII-space vocabulary and its default-mode fallback.
// Unrecognized names were ignored by the native parser; tabs are not delimiters.
func parseMaintenanceModes(csv string) uint32 {
	var modes uint32
	for _, name := range strings.FieldsFunc(csv, func(r rune) bool { return r == ',' || r == ' ' }) {
		switch name {
		case "replay":
			modes |= MaintenanceReplay
		case "compact":
			modes |= MaintenanceCompact
		case "prune":
			modes |= MaintenancePrune
		case "summarize":
			modes |= MaintenanceSummarize
		case "drift":
			modes |= MaintenanceDrift
		}
	}
	return modes
}

func addMaintenanceConsole(result map[string]any, summary MaintenanceSummary, request DataRequest) {
	handoff := !request.DryRun && request.Modes == 0 && !summary.Skipped
	result["display"] = struct {
		MaintenanceSummary
		Status      string `json:"status"`
		Owner       string `json:"vector_maintenance_owner"`
		SkippedHere bool   `json:"vector_maintenance_skipped_here"`
	}{summary, "ok", "knowledge-service", handoff}
	text := "Maintenance cycle skipped (idle guard).\n"
	if !summary.Skipped {
		suffix := ""
		if request.DryRun {
			suffix = " (dry-run)"
		}
		text = fmt.Sprintf("Maintenance: promoted=%d demoted=%d expired=%d archived=%d merged=%d rescored=%d elapsed_ms=%.2f%s\n",
			summary.Promoted, summary.Demoted, summary.Expired, summary.LifecycleArchived, summary.Merged, summary.Rescored, summary.ElapsedMS, suffix)
	}
	if handoff {
		text += "Vector maintenance skipped here; ownership belongs to the knowledge service.\n"
	}
	result["text"] = text
}
