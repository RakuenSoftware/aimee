// Package executionpolicy implements the required fail-closed tool policy module.
package executionpolicy

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventTool uint32 = 8449
	StageTool uint32 = 1
)

type computerUsePolicy struct {
	Enabled                    bool     `json:"enabled"`
	DefaultNavigation          string   `json:"default_navigation"`
	RedactSensitiveScreenshots bool     `json:"redact_sensitive_screenshots"`
	AllowedDomains             []string `json:"allowed_domains"`
}

type request struct {
	Tool        string            `json:"tool"`
	SideEffect  string            `json:"side_effect"`
	Arguments   json.RawMessage   `json:"arguments"`
	ComputerUse computerUsePolicy `json:"computer_use"`
}

type response struct {
	Allowed bool   `json:"allowed"`
	Reason  string `json:"reason"`
}

type toolRule struct {
	PathPrefix   string   `json:"path_prefix"`
	AllowedTools []string `json:"allowed_tools"`
}

type operatorPolicy struct {
	ForbiddenCommands []string          `json:"forbidden_commands"`
	ToolRules         []toolRule        `json:"tool_rules"`
	ApprovalLevels    map[string]string `json:"approval_levels"`
}

type policyLoader func() (*operatorPolicy, error)

const maxPolicyBytes = 1024 * 1024

func decodeSingle(decoder *json.Decoder, value any) error {
	if err := decoder.Decode(value); err != nil {
		return err
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("multiple JSON values")
		}
		return err
	}
	return nil
}

func defaultPolicyLoader() (*operatorPolicy, error) {
	paths := []string{".aimee-policy.json"}
	if home := os.Getenv("AIMEE_HOME"); home != "" {
		paths = append(paths, filepath.Join(home, "policy.json"))
	}
	for _, path := range paths {
		body, err := os.ReadFile(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, err
		}
		if len(body) == 0 || len(body) >= maxPolicyBytes {
			return nil, errors.New("operator policy has invalid size")
		}
		var policy operatorPolicy
		decoder := json.NewDecoder(bytes.NewReader(body))
		// Older operator policies may carry fields owned by other policy seams;
		// ignore those fields as the C implementation did, while still rejecting
		// malformed or concatenated JSON.
		if err := decodeSingle(decoder, &policy); err != nil {
			return nil, err
		}
		return &policy, nil
	}
	return nil, nil
}

func textField(arguments map[string]any, names ...string) string {
	for _, name := range names {
		if value, ok := arguments[name].(string); ok {
			return value
		}
	}
	return ""
}

func containsSensitive(value string) bool {
	lower := strings.ToLower(value)
	for _, token := range []string{"password", "login", "sign in", "payment", "pay ",
		"credit card", "cvv", "token", "secret", "upload", "download"} {
		if strings.Contains(lower, token) {
			return true
		}
	}
	return false
}

func domainAllowed(policy computerUsePolicy, host string) bool {
	host = strings.ToLower(host)
	if host == "localhost" || host == "127.0.0.1" || host == "::1" {
		return true
	}
	for _, pattern := range policy.AllowedDomains {
		pattern = strings.ToLower(pattern)
		if pattern == host || (strings.HasPrefix(pattern, "*.") &&
			strings.HasSuffix(host, strings.TrimPrefix(pattern, "*"))) {
			return true
		}
	}
	return false
}

func computerUseDecision(req request, arguments map[string]any) (bool, string, bool) {
	if !strings.HasPrefix(req.Tool, "computer_use:") && !strings.HasPrefix(req.Tool, "computer-use:") {
		return true, "", false
	}
	if !req.ComputerUse.Enabled {
		return false, "computer-use tools are disabled", true
	}
	action := req.Tool
	if index := strings.IndexByte(action, ':'); index >= 0 {
		action = action[index+1:]
	}
	lower := strings.ToLower(action)
	if strings.Contains(lower, "open") || strings.Contains(lower, "navigate") || strings.Contains(lower, "url") {
		target := textField(arguments, "url", "target", "href")
		host := ""
		if parsed, err := url.Parse(target); err == nil {
			host = parsed.Hostname()
		}
		if domainAllowed(req.ComputerUse, host) {
			return true, "navigation target is allowlisted", true
		}
		if req.ComputerUse.DefaultNavigation == "allow" {
			return true, "off-allowlist navigation allowed by policy", true
		}
		return false, "off-allowlist navigation requires approval", true
	}
	if strings.Contains(lower, "type") || strings.Contains(lower, "click") {
		if containsSensitive(textField(arguments, "field_type", "selector", "label")) ||
			containsSensitive(textField(arguments, "text", "value", "target")) {
			return false, "sensitive GUI interaction requires approval", true
		}
	}
	if strings.Contains(lower, "screenshot") &&
		containsSensitive(textField(arguments, "page_class", "classification", "label")) {
		return false, "sensitive screenshot requires approval", true
	}
	if strings.Contains(lower, "upload") || strings.Contains(lower, "download") {
		return false, "file transfer through GUI requires approval", true
	}
	return true, "computer-use observe/interact action allowed", true
}

func tokenIsDocsPath(token string) bool {
	token = strings.Trim(token, "\"'")
	return token == "docs" || strings.HasPrefix(token, "docs/") || token == "./docs" ||
		strings.HasPrefix(token, "./docs/")
}

func tokenLooksSpecificFilePath(token string) bool {
	token = strings.Trim(token, "\"'")
	if token == "" || strings.HasPrefix(token, "-") || strings.HasSuffix(token, "/") ||
		strings.Contains(token, "*") || token == "." || token == "src" ||
		strings.HasPrefix(token, "--include") {
		return false
	}
	return strings.Contains(token, ".") || token == "README" || token == "Makefile"
}

func sourceDiscovery(command string) bool {
	fields := strings.Fields(command)
	if len(fields) == 0 {
		return false
	}
	for _, field := range fields {
		if strings.HasPrefix(field, "/var/") || strings.HasPrefix(field, "/tmp/") ||
			strings.HasPrefix(field, "/proc/") || strings.HasPrefix(field, "/sys/") ||
			strings.HasPrefix(field, "/run/") || strings.HasPrefix(field, "/etc/") || tokenIsDocsPath(field) {
			return false
		}
	}
	switch fields[0] {
	case "grep", "rg", "ripgrep":
		// A concrete file after the search pattern is a read, not repository discovery.
		for _, field := range fields[2:] {
			if tokenLooksSpecificFilePath(field) {
				return false
			}
		}
		return true
	case "find":
		if len(fields) < 2 || !strings.HasPrefix(fields[1], "/") {
			return true
		}
		for _, field := range fields {
			for _, ext := range []string{"*.c", "*.h", "*.py", "*.go", "*.rs"} {
				if strings.Contains(field, ext) {
					return true
				}
			}
		}
	}
	return false
}

func evaluate(req request, policy *operatorPolicy) response {
	var arguments map[string]any
	if err := json.Unmarshal(req.Arguments, &arguments); err != nil || arguments == nil {
		return response{Reason: "tool arguments are invalid JSON"}
	}
	if allowed, reason, handled := computerUseDecision(req, arguments); handled && !allowed {
		return response{Reason: reason}
	}
	if req.Tool == "bash" && sourceDiscovery(textField(arguments, "command")) {
		return response{Reason: "Use `aimee index find <symbol>` or `aimee index overview` for code discovery. Fall back to shell search only if aimee returns nothing."}
	}
	if policy == nil {
		return response{Allowed: true, Reason: "no operator policy restriction matched"}
	}
	command := textField(arguments, "command")
	if req.Tool == "bash" {
		for _, pattern := range policy.ForbiddenCommands {
			if pattern != "" && strings.Contains(command, pattern) {
				return response{Reason: "command matches forbidden pattern: " + pattern}
			}
		}
	}
	target := textField(arguments, "path", "command")
	for _, rule := range policy.ToolRules {
		if target == rule.PathPrefix || strings.HasPrefix(target, strings.TrimSuffix(rule.PathPrefix, "/")+"/") {
			for _, allowed := range rule.AllowedTools {
				if allowed == req.Tool {
					goto approval
				}
			}
			return response{Reason: "tool '" + req.Tool + "' not allowed for path " + rule.PathPrefix}
		}
	}
approval:
	if policy.ApprovalLevels[req.SideEffect] == "block" {
		return response{Reason: "policy blocks " + req.SideEffect + " operations"}
	}
	return response{Allowed: true, Reason: "operator policy allows the action"}
}

func newHandler(load policyLoader) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != StageTool || invocation.Cancelled() {
			if invocation.Cancelled() {
				return nil, bus.ModuleStatusCancelled
			}
			return nil, bus.ModuleStatusInvalidRequest
		}
		var req request
		decoder := json.NewDecoder(bytes.NewReader(body))
		decoder.DisallowUnknownFields()
		if err := decodeSingle(decoder, &req); err != nil || req.Tool == "" || len(req.Arguments) == 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		policy, err := load()
		decision := response{}
		if err != nil {
			decision.Reason = "operator policy is unreadable or invalid"
		} else {
			decision = evaluate(req, policy)
		}
		encoded, err := json.Marshal(decision)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	}
}

// Handle evaluates one tool authorization with the local operator policy.
var Handle = newHandler(defaultPolicyLoader)
