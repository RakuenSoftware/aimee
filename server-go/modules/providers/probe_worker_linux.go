//go:build linux

package providers

// CLI diagnostics run in a disposable Go worker started before the management
// process drops ambient network access. The worker accepts only the name of a
// configured model and a fixed diagnostic prompt, never caller-supplied argv.
import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const probeWorkerArgument = "__aimee_provider_probe_worker"

type probeWorker struct {
	cmd    *exec.Cmd
	input  io.WriteCloser
	output *json.Decoder
	seat   chan struct{}
}

func newProbeWorker(ctx context.Context, home string) (*probeWorker, error) {
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	cmd := exec.CommandContext(ctx, exe, probeWorkerArgument)
	cmd.Env = append(os.Environ(), "AIMEE_HOME="+home)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
	input, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		input.Close()
		return nil, err
	}
	cmd.Stderr = os.Stderr
	if err = cmd.Start(); err != nil {
		input.Close()
		output.Close()
		return nil, err
	}
	go cmd.Wait()
	return &probeWorker{cmd: cmd, input: input, output: json.NewDecoder(output), seat: make(chan struct{}, 1)}, nil
}
func (w *probeWorker) probe(ctx context.Context, name string) (object, error) {
	select {
	case w.seat <- struct{}{}:
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	defer func() { <-w.seat }()
	type result struct {
		out object
		err error
	}
	done := make(chan result, 1)
	go func() {
		if err := json.NewEncoder(w.input).Encode(object{"name": name}); err != nil {
			done <- result{err: err}
			return
		}
		var out object
		err := w.output.Decode(&out)
		done <- result{out, err}
	}()
	select {
	case reply := <-done:
		return reply.out, reply.err
	case <-ctx.Done():
		_ = w.cmd.Process.Kill()
		_ = w.input.Close()
		<-done
		return nil, ctx.Err()
	}
}

// RunProbeWorker is called before the bus runtime and its network guard.
func RunProbeWorker(args []string) (bool, int) {
	if len(args) != 2 || args[1] != probeWorkerArgument {
		return false, 0
	}
	store, err := NewStore(os.Getenv("AIMEE_HOME"))
	if err != nil {
		return true, 1
	}
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 4096), 4096)
	for scanner.Scan() {
		var req object
		if json.Unmarshal(scanner.Bytes(), &req) != nil {
			return true, 1
		}
		reply := runConfiguredProbe(store, str(req, "name"))
		if json.NewEncoder(os.Stdout).Encode(reply) != nil {
			return true, 1
		}
	}
	return true, 0
}
func runConfiguredProbe(store *Store, name string) object {
	result := object{"execution_tested": true, "execution_ok": false}
	var model object
	_, err := store.transaction(false, func(root object) (object, error) {
		model = find(rows(root, "models"), name)
		if model == nil {
			return nil, errors.New("model not found")
		}
		model = copyObject(model)
		return nil, nil
	})
	if err != nil {
		result["execution_error"] = err.Error()
		return result
	}
	args, err := splitProbeCommand(str(model, "cli_cmd"))
	if err != nil {
		result["execution_error"] = err.Error()
		return result
	}
	kind := str(model, "cli_kind")
	if kind == "" {
		kind = str(model, "provider")
	}
	switch kind {
	case "claude", "claude-code":
		args = append(args, "-p", "--output-format", "stream-json", "--verbose", "--tools", "", "--max-turns", "1")
		if id := str(model, "model"); id != "" {
			args = append(args, "--model", id)
		}
	case "codex", "chatgpt":
		args = append(args, "exec", "--ephemeral", "--json", "--skip-git-repo-check", "--sandbox", "read-only", "--color", "never", "-")
	}
	work, err := os.MkdirTemp("", "aimee-model-probe-*")
	if err != nil {
		result["execution_error"] = "cannot create diagnostic directory"
		return result
	}
	defer os.RemoveAll(work)
	timeout := 25 * time.Second
	if n := number(model, "timeout_ms"); n > 0 && n < 25000 {
		timeout = time.Duration(n) * time.Millisecond
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, args[0], args[1:]...)
	cmd.Dir = work
	cmd.Stdin = strings.NewReader("Reply with exactly: ok\n")
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return nil
		}
		return syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
	}
	cmd.WaitDelay = time.Second
	// A bounded sink reports overflow as a failure, never as a successful prefix.
	output := &probeOutput{}
	cmd.Stdout = output
	cmd.Stderr = io.Discard
	err = cmd.Run()
	if cmd.Process != nil {
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
	}
	if err != nil {
		result["execution_error"] = "CLI diagnostic failed"
		if ctx.Err() != nil {
			result["execution_error"] = "CLI diagnostic timed out"
		}
		return result
	}
	if output.overflow {
		result["execution_error"] = "CLI diagnostic exceeded output limit"
		return result
	}
	result["execution_ok"] = validProbeOutput(kind, output.body.String())
	return result
}

type probeOutput struct {
	body     strings.Builder
	overflow bool
}

func (b *probeOutput) Write(p []byte) (int, error) {
	if b.body.Len()+len(p) > 1<<20 {
		b.overflow = true
		return len(p), nil
	}
	_, _ = b.body.Write(p)
	return len(p), nil
}
func splitProbeCommand(command string) ([]string, error) {
	args := []string{}
	var word strings.Builder
	var quote rune
	escaped := false
	flush := func() {
		if word.Len() > 0 {
			args = append(args, word.String())
			word.Reset()
		}
	}
	for _, ch := range strings.TrimSpace(command) {
		if escaped {
			word.WriteRune(ch)
			escaped = false
			continue
		}
		if ch == '\\' && quote != '\'' {
			escaped = true
			continue
		}
		if quote != 0 {
			if ch == quote {
				quote = 0
			} else {
				word.WriteRune(ch)
			}
			continue
		}
		switch ch {
		case '\'', '"':
			quote = ch
		case ' ', '\t':
			flush()
		case ';', '|', '&', '<', '>', '`', '\n', '\r':
			return nil, errors.New("invalid diagnostic command")
		default:
			word.WriteRune(ch)
		}
	}
	if escaped || quote != 0 {
		return nil, errors.New("unterminated diagnostic command")
	}
	flush()
	if len(args) == 0 {
		return nil, errors.New("CLI command is not configured")
	}
	if filepath.Base(args[0]) == "" {
		return nil, errors.New("invalid diagnostic executable")
	}
	return args, nil
}

func validProbeOutput(kind, output string) bool {
	if kind != "claude" && kind != "claude-code" && kind != "codex" && kind != "chatgpt" {
		return strings.TrimSpace(output) != ""
	}
	valid := false
	for _, line := range strings.Split(output, "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		var event object
		if json.Unmarshal([]byte(line), &event) != nil {
			return false
		}
		if boolean(event, "is_error", false) || str(event, "type") == "error" || str(event, "type") == "turn.failed" {
			return false
		}
		if str(event, "type") == "result" && str(event, "result") != "" {
			valid = true
		}
		if item, ok := event["item"].(map[string]any); ok && str(item, "type") == "agent_message" && str(item, "text") != "" {
			valid = true
		}
	}
	return valid
}
