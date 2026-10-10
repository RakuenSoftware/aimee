package memory

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

var backendCommandRoutes = []commandRoute{
	{"memory", "backend_capabilities", "Inspect the selected backend's supported operations.", handleBackendCommand, true},
	{"memory", "backend_list", "Enumerate current records by stable ID within the admitted scope.", handleBackendCommand, true},
	{"memory", "backend_export", "Export an admitted scope including versions, history and erasure metadata.", handleBackendCommand, true},
	{"memory", "backend_import", "Import an exported scope into an empty backend catalog.", handleBackendCommand, true},
}

func handleBackendCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{Operation: strings.ReplaceAll(verb, "_", "-"), Limit: args.limit("limit", 100, 1000)}
	commandScope(args, &request)
	if raw, exists := args["after_id"]; exists {
		if json.Unmarshal(raw, &request.AfterID) != nil || request.AfterID < 0 {
			return commandResult(commandError("invalid_argument", "after_id requires a nonnegative integer"))
		}
	}
	if verb == "backend_import" {
		snapshot, exists := args["snapshot"]
		if !exists || !json.Valid(snapshot) || len(snapshot) > maxDataBody/2 {
			return commandResult(commandError("invalid_argument", "snapshot requires a bounded export object"))
		}
		request.Content = string(snapshot)
	}
	raw, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if response.Failure != nil {
		return commandResult(commandError(response.Failure.Kind, response.Failure.Message))
	}
	switch verb {
	case "backend_capabilities", "backend_export":
		return commandResult(json.RawMessage(response.Payload))
	case "backend_list":
		next := request.AfterID
		for _, r := range response.Records {
			if r.ID > next {
				next = r.ID
			}
		}
		return commandResult(map[string]any{"records": response.Records, "after_id": next})
	default:
		return commandResult(map[string]any{"status": "ok", "imported": response.Deleted})
	}
}
