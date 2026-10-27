package memory

import (
	"encoding/json"
	"strings"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func handleAnswerCommand(options handlerOptions, invocation ring.ModuleInvocation, _ string, args commandArgs) ([]byte, ring.ModuleStatus) {
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
	if status != ring.ModuleStatusOK {
		if status == ring.ModuleStatusInternal || status == ring.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory retrieval unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(reply, &response) != nil || response.Answer == nil {
		return nil, ring.ModuleStatusInternal
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
