package memory

import (
	"encoding/json"
	"math"

	"github.com/JBailes/aimee/server-go/bus"
)

// Accepted learning proposals use the same memory mutations as public commands.
// Only the host can reach this path; proposal text cannot grant user authority.
func handleLearningMutation(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	var action commandArgs
	if json.Unmarshal(args["action"], &action) != nil || action == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	request := DataRequest{IncludeAll: true, SessionID: args.stringOr("session_id", "")}
	commandScope(args, &request)
	confidence := 1.0
	switch args.stringOr("sink", "") {
	case "reranker":
		request.Operation = "feedback"
		var success bool
		if json.Unmarshal(action["success"], &success) != nil {
			n, _ := action.number("success")
			success = n != 0
		}
		request.Success = success
		var items []json.RawMessage
		_ = json.Unmarshal(action["citation_ids"], &items)
		for _, raw := range items {
			var id float64
			if json.Unmarshal(raw, &id) == nil && id > 0 && id < math.MaxInt64 && math.Trunc(id) == id {
				request.IDs = append(request.IDs, int64(id))
			}
			if len(request.IDs) == 32 {
				break
			}
		}
		if len(request.IDs) == 0 {
			if id, ok := args.positiveID("target_memory_id"); ok {
				request.IDs = []int64{id}
			}
		}
		if len(request.IDs) == 0 {
			return commandResult(map[string]any{"status": "ok"})
		}
	case "supersede":
		var ok bool
		request.ID, ok = action.positiveID("old_memory_id")
		request.Content = action.stringOr("new_content", "")
		if !ok || request.Content == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		request.Operation, request.Confidence = "supersede", &confidence
		options.publicWrite = options.placement == PlacementKB
	case "workflow":
		request.Operation, request.Workspace, request.SignalType, request.Rule = "upsert-workflow", action.stringOr("project", ""), action.stringOr("signal_type", ""), action.stringOr("rule", "")
		if request.Workspace == "" || request.SignalType == "" || request.Rule == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		request.Confidence = &confidence
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if request.Operation != "feedback" {
		if response.Code != nil || len(response.Records) != 1 || response.Records[0].ID <= 0 {
			return commandResult(commandError("conflict", "learning memory mutation was refused"))
		}
		return commandResult(map[string]any{"status": "ok", "id": response.Records[0].ID})
	}
	return commandResult(map[string]any{"status": "ok"})
}
