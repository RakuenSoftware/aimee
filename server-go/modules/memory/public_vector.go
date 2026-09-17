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

func handleRepairCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	var failedOnly, reset bool
	_ = json.Unmarshal(args["failed_only"], &failedOnly)
	_ = json.Unmarshal(args["reset_stuck"], &reset)
	memoryID, validID := args.positiveID("memory_id")
	if _, present := args["memory_id"]; present && !validID {
		return commandResult(commandError("invalid_argument", "memory_id must be positive"))
	}
	request := DataRequest{Operation: "vector-repair-prepare", IncludeAll: true, ID: memoryID,
		Limit: args.limit("limit", 1024, 1024), FailedOnly: failedOnly, ResetStuck: reset}
	call := func(request DataRequest) (DataResponse, bus.ModuleStatus) {
		body, _ := json.Marshal(request)
		reply, status := handleData(options, invocation, body)
		var response DataResponse
		if status == bus.ModuleStatusOK && json.Unmarshal(reply, &response) != nil {
			status = bus.ModuleStatusInternal
		}
		return response, status
	}
	prepared, status := call(request)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	if reset {
		if prepared.Count == nil {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "mode": "reset_stuck", "reset_stuck": *prepared.Count})
	}
	backend, ok := options.data.(*postgresDataStore)
	if !ok {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	command, err := backend.embeddingCommand(args.stringOr("embedding_command", ""))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	mode := "all"
	if request.ID > 0 {
		mode = "single"
	} else if failedOnly {
		mode = "failed_only"
	}
	repaired, failed := 0, 0
	for _, id := range prepared.IDs {
		if invocation.Cancelled() || invocation.Remaining(embedHTTPTimeout()) <= 0 {
			return nil, bus.ModuleStatusCancelled
		}
		result, status := call(DataRequest{Operation: "vector-repair-record", IncludeAll: true, ID: id, Command: command, Dimension: prepared.Dimension})
		if status != bus.ModuleStatusOK {
			return nil, status
		}
		if result.Embedding == nil {
			return nil, bus.ModuleStatusInternal
		}
		if result.Embedding.Embedded {
			repaired++
		} else {
			failed++
		}
	}
	result := map[string]any{"status": "ok", "mode": mode, "repaired": repaired, "failed": failed}
	if request.ID > 0 {
		result["memory_id"] = request.ID
	}
	if n, ok := args.number("limit"); ok && n > 0 {
		result["limit"] = int(n)
	}
	if failedOnly {
		result["failed_only"] = true
	}
	return commandResult(result)
}
