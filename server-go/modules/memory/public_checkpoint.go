package memory

import (
	"encoding/json"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
)

// Checkpoint storage belongs to DB1. The shared memory owner selects its fact
// projection and applies the same scoped write policy as other memory producers.
func handleCheckpointCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	action := args.stringOr("action", "")
	request := DataRequest{Operation: "list", Tier: "L2", Kind: "fact", Limit: 16}
	switch action {
	case "facts":
	case "restore":
		id, ok := args.positiveID("checkpoint_id")
		if decimal, isString := args.stringValue("checkpoint_id"); isString {
			var err error
			id, err = strconv.ParseInt(decimal, 10, 64)
			ok = err == nil && id > 0 && strconv.FormatInt(id, 10) == decimal
		}
		snapshot, valid := args.stringValue("snapshot")
		if !ok || !valid || snapshot == "" {
			return commandResult(commandError("invalid_argument", "checkpoint restore requires a positive checkpoint_id and snapshot"))
		}
		confidence := 1.0
		request = DataRequest{Operation: "insert-epistemic", Tier: "L0", Kind: "scratch", EpistemicKind: "world_fact",
			Key: "checkpoint_restore:" + strconv.FormatInt(id, 10), Content: snapshot, Confidence: &confidence,
			Authority: AuthorityModel, SessionID: args.stringOr("session_id", "")}
		options.publicWrite = true
	default:
		return commandResult(commandError("invalid_argument", "checkpoint action must be facts or restore"))
	}
	commandScope(args, &request)
	body, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "checkpoint memory operation failed"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	if refusal := commandMutationRefusal(response.Code, response.Proposal); refusal != nil {
		return commandResult(refusal)
	}
	if action == "restore" {
		if len(response.Records) != 1 || response.Records[0].ID <= 0 {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "memory_id": strconv.FormatInt(response.Records[0].ID, 10)})
	}
	type fact struct {
		Key     string `json:"key"`
		Content string `json:"content"`
	}
	facts := make([]fact, 0, len(response.Records))
	for _, record := range response.Records {
		facts = append(facts, fact{record.Key, record.Content})
	}
	return commandResult(map[string]any{"status": "ok", "facts": facts})
}
