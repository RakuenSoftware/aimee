package memory

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleScopeCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "scope_visibility_rank":
		var ids []json.RawMessage
		if string(args["ids"]) == "null" || json.Unmarshal(args["ids"], &ids) != nil {
			return invalid("missing ids array")
		}
		if len(ids) > 256 {
			ids = ids[:256]
		}
		for _, raw := range ids {
			id, _ := commandArgs{"id": raw}.positiveID("id")
			request.IDs = append(request.IDs, id)
		}
		if len(ids) == 0 {
			return commandResult(map[string]any{"status": "ok", "ranks": []int{}})
		}
		request.Operation, request.IncludeAll = "scope-rank", false
		request.Workspace, request.Project = args.stringOr("workspace", ""), args.stringOr("project", "")
	case "tag_workspace", "tag_scope":
		var idOK, kindOK, valueOK bool
		request.ID, idOK = args.positiveID("memory_id")
		kind := "workspace"
		var value string
		if verb == "tag_workspace" {
			kindOK = true
			value, valueOK = args.stringValue("workspace")
		} else {
			kind, kindOK = args.stringValue("scope_type")
			value, valueOK = args.stringValue("scope_value")
		}
		if !idOK || !kindOK || !valueOK {
			return invalid("missing memory_id or scope")
		}
		if _, err := normalizeScope(PlacementKB, Scope{Type: kind, Value: value}); err != nil {
			return invalid("invalid memory scope")
		}
		request.Operation, request.TagScope = "scope-tag", &Scope{Type: kind, Value: value}
		scoped = commandScope(args, &request)
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
	if verb == "scope_visibility_rank" {
		ranks := make([]int, len(response.ScopeRanks))
		for i, r := range response.ScopeRanks {
			ranks[i] = r.Rank
		}
		result["ranks"] = ranks
	} else if !response.Updated {
		return commandResult(commandError("not_found", "memory not found"))
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}
