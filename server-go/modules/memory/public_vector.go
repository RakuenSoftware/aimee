package memory

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleVectorCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	switch verb {
	case "reindex":
		request.Operation, request.Limit = "rebuild-derived", args.limit("limit", 100000, 100000)
	case "rebuild":
		request.Operation, request.Version = "vector-rebuild", strings.TrimSpace(args.stringOr("version", ""))
		if len(request.Version) > 256 {
			return commandResult(commandError("invalid_argument", "embedder version exceeds bounds"))
		}
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, _ := json.Marshal(request)
	reply, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(reply, &response) != nil || response.Count == nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok", "rebuilt": *response.Count}
	if verb == "rebuild" {
		result["version"], result["failed"] = response.Version, response.Failed
	}
	return commandResult(result)
}
