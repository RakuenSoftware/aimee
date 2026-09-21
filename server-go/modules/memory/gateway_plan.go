package memory

import (
	"encoding/json"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

type gatewayStep struct {
	Kind        string         `json:"kind"`
	Binding     string         `json:"binding,omitempty"`
	Args        map[string]any `json:"args,omitempty"`
	Output      string         `json:"output,omitempty"`
	Resource    string         `json:"resource,omitempty"`
	WhenOutput  string         `json:"when_output,omitempty"`
	EpochOutput string         `json:"epoch_output,omitempty"`
	Context     map[string]any `json:"context,omitempty"`
	Indices     []int          `json:"indices,omitempty"`
}

func gatewayOptionalString(args commandArgs, key string) (string, bool) {
	raw, exists := args[key]
	if !exists {
		return "", true
	}
	var value *string
	if json.Unmarshal(raw, &value) != nil || value == nil {
		return "", false
	}
	return *value, true
}

func handleGatewayPlan(options handlerOptions, args commandArgs) ([]byte, bus.ModuleStatus) {
	phase := "preview"
	if _, exists := args["phase"]; exists {
		var ok bool
		phase, ok = gatewayOptionalString(args, "phase")
		if !ok {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if phase != "preview" && phase != "context" && phase != "tools" && phase != "text" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// These are host assembly facts, not labels parsed from user/model text.
	// In particular a host-composed persona already contains standing guidance.
	provided := []string{}
	if raw, exists := args["provided_resources"]; exists {
		if json.Unmarshal(raw, &provided) != nil || provided == nil || len(provided) > 64 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	guidanceProvided := false
	for _, name := range provided {
		if name == "" || len(name) > 64 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		guidanceProvided = guidanceProvided || name == "guidance"
	}
	roles, tools := []string{}, []string{}
	if phase != "text" {
		if json.Unmarshal(args["roles"], &roles) != nil || roles == nil || json.Unmarshal(args["tools"], &tools) != nil || tools == nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	start := true
	for _, role := range roles {
		if role == "assistant" {
			start = false
			break
		}
	}
	remove := []int{}
	if start {
		for i := len(tools) - 1; i >= 0; i-- {
			switch tools[i] {
			case "exec_command", "local_shell", "shell", "bash", "run_command", "container.exec":
				remove = append(remove, i)
			}
		}
	}
	steps := []gatewayStep{}
	appendGuidance := start && !guidanceProvided
	result := map[string]any{"status": "ok", "append_guidance": appendGuidance, "remove_tools": remove}
	if phase == "tools" {
		if len(remove) > 0 {
			steps = append(steps, gatewayStep{Kind: "remove_tools", Indices: remove})
		}
	} else if phase == "context" || phase == "text" {
		provided, ok := gatewayOptionalString(args, "provided_query")
		if !ok {
			return nil, bus.ModuleStatusInvalidRequest
		}
		fallback, ok := gatewayOptionalString(args, "last_user_text")
		if !ok {
			return nil, bus.ModuleStatusInvalidRequest
		}
		query := provided
		if query == "" {
			query = fallback
			if len(query) > 16383 {
				end := 16383
				for end > 0 && !utf8.RuneStart(query[end]) {
					end--
				}
				query = query[:end]
			}
		}
		if query != "" {
			gate := gatewayRecall(options.gateway, query)
			if message, ok := gate["log"]; ok {
				steps = append(steps, gatewayStep{Kind: "invoke", Binding: "log", Args: map[string]any{"module": "memory", "message": message}})
			}
			if role, ok := gate["audit_role"]; ok {
				steps = append(steps, gatewayStep{Kind: "invoke", Binding: "audit", Args: map[string]any{"role": role, "query_fingerprint": gate["query_fingerprint"]}})
			}
			if gate["enforced"] != true {
				steps = append(steps, gatewayStep{Kind: "invoke", Binding: "context", Args: map[string]any{"query": query}, Output: "evidence"})
			}
		}
		if phase == "context" {
			if appendGuidance {
				steps = append(steps, gatewayStep{Kind: "append_context", Resource: "guidance", Context: map[string]any{"origin": "platform", "authority": "task_instruction", "trust": "verified", "sensitivity": "internal", "model_visible": true}})
			}
			steps = append(steps,
				gatewayStep{Kind: "invoke", Binding: "epoch", Args: map[string]any{"domain": "knowledge", "scope": "global"}, Output: "epoch", WhenOutput: "evidence"},
				gatewayStep{Kind: "append_context", Output: "evidence", WhenOutput: "evidence", EpochOutput: "epoch", Context: map[string]any{"origin": "retrieval", "authority": "evidence", "trust": "unverified", "sensitivity": "internal", "model_visible": true, "revision_domain": "knowledge", "revision_scope": "global"}},
			)
		}
		result["result"] = "evidence"
	}
	result["steps"] = steps
	return commandResult(result)
}
