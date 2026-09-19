package memory

import (
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

// Recognize literal invocations, without executing shell text. Quoted arguments
// remain single words, so mentions in echo/grep are not observed as commands.
// An incomplete quote is ambiguous and produces no learning.
func workflowInvocations(command string) [][]string {
	var commands [][]string
	var words []string
	var word strings.Builder
	started := false
	var quote byte
	flushWord := func() {
		if started {
			words = append(words, word.String())
			word.Reset()
			started = false
		}
	}
	flushCommand := func() {
		flushWord()
		if len(words) > 0 {
			commands = append(commands, words)
			words = nil
		}
	}
	for i := 0; i < len(command); i++ {
		c := command[i]
		if c == '\\' && quote != '\'' {
			if i+1 == len(command) {
				return nil
			}
			i++
			if command[i] != '\n' {
				word.WriteByte(command[i])
				started = true
			}
			continue
		}
		if quote != 0 {
			if c == quote {
				quote = 0
			} else {
				word.WriteByte(c)
			}
			continue
		}
		switch c {
		case '\'', '"':
			quote = c
			started = true
		case ' ', '\t', '\r':
			flushWord()
		case ';', '&', '|', '\n', '(', ')':
			flushCommand()
		case '#':
			if !started {
				for i < len(command) && command[i] != '\n' {
					i++
				}
				flushCommand()
			} else {
				word.WriteByte(c)
			}
		default:
			word.WriteByte(c)
			started = true
		}
	}
	if quote != 0 {
		return nil
	}
	flushCommand()
	return commands
}

func workflowLiteralBranch(branch string) bool {
	return branch != "" && len(branch) <= 127 && utf8.ValidString(branch) && !strings.HasPrefix(branch, "-") && !strings.ContainsAny(branch, " \t\r\n$`\\;|&()<>\"'")
}

func parseWorkflowSignal(command string) (string, string) {
	commands := workflowInvocations(command)
	// Preserve the original preference for PR conventions, then active branches,
	// then test commands when several invocations occur in the same shell text.
	for _, words := range commands {
		if len(words) < 3 || words[0] != "gh" || words[1] != "pr" || words[2] != "create" {
			continue
		}
		for i := 3; i < len(words); i++ {
			if words[i] == "--" {
				break
			}
			base := ""
			if words[i] == "--base" && i+1 < len(words) {
				base = words[i+1]
			} else if strings.HasPrefix(words[i], "--base=") {
				base = strings.TrimPrefix(words[i], "--base=")
			}
			if workflowLiteralBranch(base) {
				return "pr-target", fmt.Sprintf("PRs target the `%s` branch (learned from `gh pr create --base %s`).", base, base)
			}
		}
	}
	for _, words := range commands {
		if len(words) < 4 || words[0] != "git" || words[1] != "push" {
			continue
		}
		i := 2
		for i < len(words) && strings.HasPrefix(words[i], "-") {
			i++
		}
		i++ // remote
		if i < len(words) {
			branch := words[i]
			if workflowLiteralBranch(branch) && branch != "main" && branch != "master" && branch != "HEAD" {
				return "active-branch", fmt.Sprintf("Active development on branch `%s` (observed via `git push origin %s`).", branch, branch)
			}
		}
	}
	for _, words := range commands {
		for _, label := range []string{"make unit-tests", "make check", "make test", "npm test", "pnpm test", "yarn test", "cargo test", "go test", "pytest", "ctest"} {
			tokens := strings.Split(label, " ")
			if len(words) >= len(tokens) && words[0] == tokens[0] && (len(tokens) == 1 || words[1] == tokens[1]) {
				return "test-command", "Test command: `" + label + "`"
			}
		}
	}
	return "", ""
}

// Use configured order for overlapping roots, as the former host did. Match a
// path component, never a prefix sibling, and never truncate workspace identity.
func workflowWorkspace(cwd string, roots []string) string {
	for _, root := range roots {
		if root != "" && (cwd == root || strings.HasPrefix(cwd, root+"/")) {
			label := filepath.Base(root)
			if label != "." && label != "/" && len(label) <= 1024 {
				return label
			}
		}
	}
	return ""
}

func handleWorkflowObserver(operation string, args commandArgs) ([]byte, bus.ModuleStatus) {
	fail := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(map[string]any{"status": "error", "output": "error: " + message})
	}
	if operation == "workflow-result" {
		var request commandArgs
		if json.Unmarshal(args["request"], &request) != nil || request.stringOr("workspace", "") == "" || request.stringOr("signal_type", "") == "" || request.stringOr("rule", "") == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var receipt commandArgs
		if json.Unmarshal([]byte(args.stringOr("receipt", "")), &receipt) != nil || receipt.stringOr("status", "") != "ok" {
			return fail("failed to store workflow memory")
		}
		var id int64
		if json.Unmarshal(receipt["id"], &id) != nil || id <= 0 {
			return fail("failed to store workflow memory")
		}
		workspace, signal, rule := request.stringOr("workspace", ""), request.stringOr("signal_type", ""), request.stringOr("rule", "")
		return commandResult(map[string]any{"status": "ok", "learned": true, "workspace": workspace, "signal_type": signal, "rule": rule, "output": fmt.Sprintf("Stored workflow:%s:%s (memory id %d)", workspace, signal, id)})
	}
	mode := args.stringOr("mode", "")
	if mode != "observe" && mode != "explicit" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var roots []string
	if json.Unmarshal(args["workspaces"], &roots) != nil || len(roots) > 256 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	cwd := args.stringOr("cwd", "")
	if len(cwd) > 4096 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	for _, root := range roots {
		if len(root) > 4096 {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	var workspace, signal, rule, session string
	confidence := 0.6
	if mode == "observe" {
		command := args.stringOr("command", "")
		if len(command) > 64*1024 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		signal, rule = parseWorkflowSignal(command)
		session = "post_tool_update"
		if signal == "" {
			return commandResult(map[string]any{"status": "ok", "learned": false})
		}
	} else {
		signal, rule = args.stringOr("signal_type", ""), args.stringOr("rule", "")
		if signal == "" || rule == "" {
			return fail("missing 'rule' and/or 'signal_type' parameter")
		}
		workspace, session = args.stringOr("project", ""), args.stringOr("session_id", "")
		confidence = 1
	}
	if workspace == "" {
		workspace = workflowWorkspace(cwd, roots)
	}
	if workspace == "" {
		if mode == "observe" {
			return commandResult(map[string]any{"status": "ok", "learned": false})
		}
		return fail("no workspace determined from cwd; pass 'project' explicitly")
	}
	if len(workspace) > 1024 || len(signal) > 64 || len(rule) > 512*1024 || len(session) > 256 {
		return fail("workflow input exceeds bounds")
	}
	var err error
	rule, err = screenMemoryWrite("workflow:"+workspace+":"+signal, rule)
	if err != nil {
		return fail("sensitive workflow content refused")
	}
	request := map[string]any{"workspace": workspace, "signal_type": signal, "rule": rule, "observed_confidence": confidence, "session_id": session}
	return commandResult(map[string]any{"status": "ok", "request": request})
}
