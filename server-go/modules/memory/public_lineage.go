package memory

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
)

func handleLineageCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	var ok bool
	args, ok = commandDomainArgs(args, "memory.evidence")
	if !ok {
		return invalid("invalid evidence envelope")
	}
	for key, raw := range args {
		switch key {
		case "id":
		case "scope_context", "include_all":
			var value *bool
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return invalid(key + " must be boolean")
			}
		case "scope":
			var value *Scope
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return invalid("scope must be an object")
			}
		case "project", "workspace", "scope_type", "scope_value", "store", "cwd", "operation", "format", "view":
			var value *string
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return invalid(key + " must be a string")
			}
		default:
			return invalid("unrecognized evidence argument")
		}
	}
	id, ok := args.decimalID("id")
	if !ok {
		return invalid("evidence requires a positive integer id")
	}
	if value, exists := args["view"]; exists && string(value) != `"server"` {
		return invalid("invalid evidence view")
	}
	request := DataRequest{Operation: "evidence", ID: id}
	if options.placement == PlacementServer {
		if value, exists := args["store"]; exists && string(value) != `"user"` {
			return invalid("personal evidence requires store=user")
		}
		request.Scope = Scope{Type: ScopeUser}
	} else {
		if value, exists := args["store"]; exists && string(value) != `"kb"` {
			return invalid("shared evidence requires store=kb")
		}
		commandScope(args, &request)
		if raw, exists := args["scope"]; exists {
			if request.Project != "" || request.Workspace != "" || request.Scope.Type != "" {
				return invalid("scope cannot be combined with another audience")
			}
			if json.Unmarshal(raw, &request.Scope) != nil || request.Scope.Type == "" {
				return invalid("scope requires a type")
			}
		}
	}
	raw, _ := json.Marshal(request)
	body, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "evidence owner unavailable"))
	}
	var response DataResponse
	if json.Unmarshal(body, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
