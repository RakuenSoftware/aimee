package memory

import (
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

func handleFactCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	if verb != "retract" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	source, sourceOK := args.stringValue("source")
	relation, relationOK := args.stringValue("relation")
	if !sourceOK || source == "" || !relationOK || relation == "" {
		return commandResult(commandError("invalid_argument", "facts.retract requires source and relation"))
	}
	for _, key := range []string{"target", "authority"} {
		if _, exists := args[key]; exists {
			if _, ok := args.stringValue(key); !ok {
				return commandResult(commandError("invalid_argument", key+" must be a string"))
			}
		}
	}
	request := DataRequest{Operation: "fact-retract", FactSource: source, FactTarget: args.stringOr("target", ""), Relation: normalizeRelType(relation)}
	if request.Relation == "" || strings.ContainsRune(source, 0) || strings.ContainsRune(request.FactTarget, 0) || len(source) > 1024 || len(request.FactTarget) > 1024 {
		return commandResult(commandError("invalid_argument", "invalid fact selector"))
	}
	if args.stringOr("authority", "") == "user" {
		request.Authority = AuthorityUser
	}
	commandScope(args, &request)
	raw, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "fact retraction unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(reply, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
