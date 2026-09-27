package memory

import (
	"encoding/json"
	"os"
)

// The C host transports its existing Go-issued execution binding. Public
// request bodies and source channels cannot manufacture a task identity.
// The receipt binding pins this optional metadata to the exact prepared input.
type healthExecutionContext struct {
	PreviousTurn      string `json:"previous_turn,omitempty"`
	PredecessorSource string `json:"predecessor_source,omitempty"`
	Binding           string `json:"receipt_binding_sha256"`
	Task              string `json:"task"`
	Turn              string `json:"turn"`
	QueryClass        string `json:"query_class"`
	Source            string `json:"source"`
}

func (c healthExecutionContext) valid(binding string) bool {
	return (c.PreviousTurn == "" || receiptDigestValid(c.PreviousTurn) && c.PreviousTurn != c.Turn && c.PredecessorSource == "receipt_owner_completed_native_turn_v1") && c.Binding == binding && receiptDigestValid(binding) && receiptDigestValid(c.Task) && receiptDigestValid(c.Turn) && c.Source == "host_exploration_binding" && (c.QueryClass == "typed_requirements" || c.QueryClass == "unclassified")
}
func receiptMetadataWithExecution(entry *sourceReleaseEntry, args commandArgs, binding providerReceiptBinding, digest string) json.RawMessage {
	metadata := receiptMetadataWithHealth(entry)
	turn := args.stringOr("health_turn_id", binding.TurnID)
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") != "1" || turn == "" || len(turn) > 128 || binding.RequestID == "" || len(binding.RequestID) > 256 {
		return metadata
	}
	raw := args.stringOr("health_execution_binding", "")
	if len(raw) == 0 || len(raw) > 4096 {
		return metadata
	}
	var execution struct {
		Principal string `json:"principal"`
		Session   string `json:"session"`
		Task      string `json:"task"`
		Project   string `json:"project"`
		Workspace string `json:"workspace"`
	}
	if json.Unmarshal([]byte(raw), &execution) != nil || execution.Principal == "" || execution.Principal != args.stringOr("principal", "") || execution.Project != binding.Project || execution.Workspace != binding.Workspace || execution.Session == "" || len(execution.Session) > 128 || execution.Task == "" || len(execution.Task) > 128 {
		return metadata
	}
	class := "unclassified"
	for _, part := range entry.assemblyParts {
		if part.CoverageStatus != "" && part.RequirementDigest != "" {
			class = "typed_requirements"
		}
	}
	task := releaseDigest([]string{"health-owned-task-v1", execution.Principal, execution.Session, execution.Task, binding.Project, binding.Workspace})
	c := healthExecutionContext{Binding: digest, Task: task, Turn: releaseDigest([]string{"health-owned-ingress-turn-v1", task, binding.RequestID, turn}), QueryClass: class, Source: "host_exploration_binding"}
	if !c.valid(digest) {
		return metadata
	}
	var fields map[string]json.RawMessage
	if len(metadata) > 0 && json.Unmarshal(metadata, &fields) != nil {
		return metadata
	}
	if fields == nil {
		fields = map[string]json.RawMessage{}
	}
	fields["health_execution"], _ = json.Marshal(c)
	result, err := json.Marshal(fields)
	if err != nil || len(result) > 12000 {
		return metadata
	}
	return result
}
