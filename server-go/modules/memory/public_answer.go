package memory

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleAnswerCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	query, ok := args.stringValue("query")
	if !ok || strings.TrimSpace(query) == "" {
		return commandResult(commandError("invalid_argument", "memory.ask requires query"))
	}
	request := DataRequest{Operation: "ask", Query: query, Limit: args.limit("limit", 5, 8), IncludeAll: true}
	request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
	scoped := false
	if request.Scope.Type == "" && request.Scope.Value == "" {
		scoped = commandScope(args, &request)
	} else {
		request.IncludeAll = false
		if _, err := normalizeScope(PlacementKB, request.Scope); err != nil {
			return commandResult(commandError("invalid_argument", err.Error()))
		}
	}
	encoded, _ := json.Marshal(request)
	reply, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory retrieval unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(reply, &response) != nil || response.Answer == nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{}
	raw, _ := json.Marshal(response.Answer)
	_ = json.Unmarshal(raw, &result)
	delete(result, "error")
	result["status"] = "ok"
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}
