package memory

import (
	"encoding/json"
	"strings"
	"unicode/utf8"

	canonical "github.com/JBailes/aimee/server-go/ir"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
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
	Paths       [][]int        `json:"paths,omitempty"`
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

// MCP groups retain their callable surface. Native shell aliases, including namespaced
// Codex functions and Anthropic's Bash, share one opening-turn policy.
func gatewayNativeShell(tool canonical.PolicyTool, namespace string) bool {
	if strings.HasPrefix(namespace, "mcp__") {
		return false
	}
	if tool.Kind == "hosted" && tool.NativeType == "local_shell" {
		return true
	}
	if namespace == "container" && tool.Name == "exec" {
		return true
	}
	switch tool.Name {
	case "exec_command", "local_shell", "shell", "bash", "run_command", "container.exec", "shell_command", "run_shell_command", "Bash", "Shell":
		return true
	}
	return false
}

func handleGatewayPlan(options handlerOptions, args commandArgs) ([]byte, ring.ModuleStatus) {
	phase := "preview"
	if _, exists := args["phase"]; exists {
		var ok bool
		phase, ok = gatewayOptionalString(args, "phase")
		if !ok {
			return nil, ring.ModuleStatusInvalidRequest
		}
	}
	if phase != "preview" && phase != "context" && phase != "tools" && phase != "text" {
		return nil, ring.ModuleStatusInvalidRequest
	}
	// These are host assembly facts, not labels parsed from user/model text.
	// In particular a host-composed persona already contains standing guidance.
	provided := []string{}
	if raw, exists := args["provided_resources"]; exists {
		if json.Unmarshal(raw, &provided) != nil || provided == nil || len(provided) > 64 {
			return nil, ring.ModuleStatusInvalidRequest
		}
	}
	guidanceProvided := false
	for _, name := range provided {
		if name == "" || len(name) > 64 {
			return nil, ring.ModuleStatusInvalidRequest
		}
		guidanceProvided = guidanceProvided || name == "guidance"
	}
	var request *canonical.PolicyView
	if phase != "text" {
		if args["roles"] != nil || args["tools"] != nil || args["last_user_text"] != nil {
			return nil, ring.ModuleStatusInvalidRequest
		}
		var err error
		request, err = canonical.DecodePolicyView(args["request"])
		if err != nil {
			return nil, ring.ModuleStatusInvalidRequest
		}
	}
	start := true
	if request != nil {
		for _, message := range request.Messages {
			if message.Role == "assistant" {
				start = false
				break
			}
		}
	}
	remove := [][]int{}
	if start && request != nil {
		for i := len(request.Tools) - 1; i >= 0; i-- {
			tool := request.Tools[i]
			if tool.Kind == "namespace" {
				for j := len(tool.Children) - 1; j >= 0; j-- {
					if gatewayNativeShell(tool.Children[j], tool.Name) {
						remove = append(remove, []int{i, j})
					}
				}
			} else if gatewayNativeShell(tool, "") {
				remove = append(remove, []int{i})
			}
		}
	}
	steps := []gatewayStep{}
	appendGuidance := start && !guidanceProvided
	result := map[string]any{"status": "ok", "append_guidance": appendGuidance, "remove_tool_paths": remove}
	if phase == "tools" {
		if len(remove) > 0 {
			steps = append(steps, gatewayStep{Kind: "remove_tools", Paths: remove})
		}
	} else if phase == "context" || phase == "text" {
		provided, ok := gatewayOptionalString(args, "provided_query")
		if !ok {
			return nil, ring.ModuleStatusInvalidRequest
		}
		fallback, ok := gatewayOptionalString(args, "last_user_text")
		if request != nil {
			fallback = request.LastUserText
		}
		if !ok {
			return nil, ring.ModuleStatusInvalidRequest
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
