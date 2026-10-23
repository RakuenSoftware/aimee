package delegates

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
	"golang.org/x/sys/unix"
)

const maxExecutorOutput = 16 << 20

var errNoConfiguredAgents = errors.New("agent registry has no agents")

const (
	watchdogArgument = "__aimee_delegate_watchdog"
	watchdogArgvEnv  = "AIMEE_DELEGATE_WATCHDOG_ARGV"
)

type Executor interface {
	Execute(context.Context, delegatecontract.Invocation) delegatecontract.InvocationResult
}

// compatibleBool preserves the roster format accepted by the C routing side.
// Current writers emit JSON booleans, while pre-migration agents.json files may
// contain 0/1. Reject every other shape so malformed operator configuration
// still fails loudly instead of silently enabling an agent.
type compatibleBool bool

func (b *compatibleBool) UnmarshalJSON(data []byte) error {
	switch string(bytes.TrimSpace(data)) {
	case "true", "1":
		*b = true
		return nil
	case "false", "0":
		*b = false
		return nil
	default:
		return fmt.Errorf("expected boolean or legacy 0/1, got %s", data)
	}
}

// NewDefaultHandler is the independently exported delegates process entry
// point. The shared multicall binary performs the same construction explicitly
// so it can report registry errors in its process log.
func NewDefaultHandler() bus.ModuleHandler {
	executor, err := NewRegistryExecutor(os.Getenv("AIMEE_HOME"))
	if err != nil {
		return NewHandler(nil)
	}
	return NewHandler(executor)
}

type agentEntry struct {
	Name        string          `json:"name"`
	Model       string          `json:"model"`
	Backend     string          `json:"backend"`
	Provider    string          `json:"provider"`
	CLIKind     string          `json:"cli_kind"`
	CLICmd      string          `json:"cli_cmd"`
	Enabled     *compatibleBool `json:"enabled"`
	PrimaryOnly bool            `json:"primary_only"`
	Roles       []string        `json:"roles"`
	Personas    []string        `json:"personas"`
	MaxParallel *int            `json:"max_parallel"`
	Available   *compatibleBool `json:"delegate_available"`
}

type agentRegistry struct {
	DefaultAgent string       `json:"default_agent"`
	Agents       []agentEntry `json:"agents"`
	// The same roster under its current key. `aimee model local` writes
	// models.json with the entries under "models"; the older agents.json put
	// them under "agents". This executor prefers models.json, so reading only
	// "agents" meant the file it chose first was the one it could not read:
	// registering a model the documented way left every delegation refused with
	// "agent registry has no agents", and delegation only worked on a machine
	// old enough to still have an agents.json.
	Models []agentEntry `json:"models"`
}

// roster is the entry list under whichever key this file uses. Both spellings
// carry the same entry shape, so the reader does not care which it got.
func (r agentRegistry) roster() []agentEntry {
	if len(r.Agents) > 0 {
		return r.Agents
	}
	return r.Models
}

// RegistryExecutor selects the current configured registration. Native providers
// execute through the canonical, source-scoped C host owner; only explicitly
// configured generic text transports execute argv locally without a shell.
type RegistryExecutor struct {
	host     delegatecontract.StageCaller
	home     string
	path     string
	fileInfo os.FileInfo
	digest   [sha256.Size]byte
	mu       sync.Mutex
	registry agentRegistry
	stamp    int64
	limitMu  sync.Mutex
	limits   map[string]*agentLimiter
}

type agentLimiter struct {
	mu      sync.Mutex
	max     int
	inUse   int
	changed chan struct{}
}

func NewRegistryExecutor(home string) (*RegistryExecutor, error) {
	if home == "" {
		return nil, errors.New("AIMEE_HOME is required for delegate execution")
	}
	r := &RegistryExecutor{home: home, limits: make(map[string]*agentLimiter)}
	if _, err := r.load(); err != nil && !errors.Is(err, os.ErrNotExist) && !errors.Is(err, errNoConfiguredAgents) {
		return nil, err
	}
	return r, nil
}

func (r *RegistryExecutor) load() (agentRegistry, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	// Setup may create models.json after this long-lived executor starts. Resolve
	// the authoritative owner on every load; only an absent canonical entry may
	// select the legacy file. A broken symlink is an error, not legacy authority.
	path := filepath.Join(r.home, "models.json")
	file, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		if _, entryErr := os.Lstat(path); !errors.Is(entryErr, os.ErrNotExist) {
			// The canonical entry exists but is unusable; do not let constructor
			// treat its target ENOENT as an unconfigured first boot.
			return agentRegistry{}, fmt.Errorf("open canonical agent registry entry: %v", err)
		}
		path = filepath.Join(r.home, "agents.json")
		file, err = os.Open(path)
	}
	if err != nil {
		return agentRegistry{}, fmt.Errorf("open agent registry: %w", err)
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return agentRegistry{}, fmt.Errorf("stat agent registry: %w", err)
	}
	body, err := io.ReadAll(file)
	if err != nil {
		return agentRegistry{}, fmt.Errorf("read agent registry: %w", err)
	}
	// Bind the cache to the selected owner and actual file bytes. Atomic owner
	// writes and in-place rewrites can preserve timestamps and file sizes.
	digest := sha256.Sum256(body)
	if r.path == path && r.fileInfo != nil && os.SameFile(r.fileInfo, info) &&
		r.stamp == info.ModTime().UnixNano() && r.fileInfo.Size() == info.Size() &&
		r.digest == digest && len(r.registry.roster()) > 0 {
		return r.registry, nil
	}
	var registry agentRegistry
	if err := json.Unmarshal(body, &registry); err != nil {
		return agentRegistry{}, fmt.Errorf("decode agent registry: %w", err)
	}
	if len(registry.roster()) == 0 {
		return agentRegistry{}, errNoConfiguredAgents
	}
	for _, agent := range registry.roster() {
		if agent.MaxParallel != nil && *agent.MaxParallel < 0 {
			return agentRegistry{}, fmt.Errorf("agent %q has negative max_parallel", agent.Name)
		}
	}
	r.registry, r.path, r.fileInfo, r.stamp, r.digest = registry, path, info, info.ModTime().UnixNano(), digest
	return registry, nil
}

func enabled(a agentEntry) bool { return a.Enabled == nil || bool(*a.Enabled) }

func available(a agentEntry) bool { return a.Available == nil || bool(*a.Available) }

func containsOrWildcard(values []string, wanted string, emptyMeansAll bool) bool {
	if len(values) == 0 {
		return emptyMeansAll
	}
	for _, value := range values {
		if value == "all" || value == wanted {
			return true
		}
	}
	return false
}

func eligible(a agentEntry, role, persona string) bool {
	return containsOrWildcard(a.Roles, role, false) && containsOrWildcard(a.Personas, persona, true)
}

func agentMaxParallel(a agentEntry) int {
	if a.MaxParallel == nil {
		return 3 // AGENT_DEFAULT_MAX_PARALLEL
	}
	return *a.MaxParallel // zero is the configured unlimited form
}

func (r *RegistryExecutor) limiter(agent agentEntry) *agentLimiter {
	r.limitMu.Lock()
	defer r.limitMu.Unlock()
	if r.limits == nil {
		r.limits = make(map[string]*agentLimiter)
	}
	limiter := r.limits[agent.Name]
	if limiter == nil {
		limiter = &agentLimiter{changed: make(chan struct{})}
		r.limits[agent.Name] = limiter
	}
	limiter.mu.Lock()
	max := agentMaxParallel(agent)
	if limiter.max != max {
		limiter.max = max
		close(limiter.changed)
		limiter.changed = make(chan struct{})
	}
	limiter.mu.Unlock()
	return limiter
}

func (l *agentLimiter) acquire(ctx context.Context) (func(), error) {
	for {
		l.mu.Lock()
		if l.max <= 0 || l.inUse < l.max {
			l.inUse++
			l.mu.Unlock()
			var once sync.Once
			return func() {
				once.Do(func() {
					l.mu.Lock()
					l.inUse--
					close(l.changed)
					l.changed = make(chan struct{})
					l.mu.Unlock()
				})
			}, nil
		}
		changed := l.changed
		l.mu.Unlock()
		select {
		case <-ctx.Done():
			if errors.Is(ctx.Err(), context.DeadlineExceeded) {
				return nil, errors.Join(delegatecontract.ErrDelegateCapacityDeadline, ctx.Err())
			}
			return nil, ctx.Err()
		case <-changed:
		}
	}
}

func (l *agentLimiter) occupancy() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.inUse
}

func selectorUnbound(selector string) bool {
	return selector == "" || selector == "$random"
}

type groupCandidate struct {
	agent    agentEntry
	provider string
	model    string
	active   int
	max      int
}

func groupCandidateLess(a, b groupCandidate, providerUses, modelUses, agentUses map[string]int) bool {
	as := providerUses[a.provider] + modelUses[a.model]
	bs := providerUses[b.provider] + modelUses[b.model]
	if as != bs {
		return as < bs
	}
	if agentUses[a.agent.Name] != agentUses[b.agent.Name] {
		return agentUses[a.agent.Name] < agentUses[b.agent.Name]
	}
	return a.agent.Name < b.agent.Name
}

// PlanGroup chooses the complete seat assignment before any provider work is
// launched. It preserves role/persona eligibility, primary exclusion,
// provider/model diversity, current occupancy, explicit pins and per-agent
// capacity. Returned agent names are opaque participant handles to callers.
func (r *RegistryExecutor) PlanGroup(ctx context.Context,
	seats []delegatecontract.GroupPlanSeat) ([]string, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	registry, err := r.load()
	if err != nil {
		return nil, err
	}
	candidates := make([]groupCandidate, 0, len(registry.roster()))
	bySelector := make(map[string]groupCandidate)
	for _, agent := range registry.roster() {
		if !enabled(agent) || !available(agent) || agent.PrimaryOnly || strings.TrimSpace(agent.CLICmd) == "" {
			continue
		}
		provider, model := strings.TrimSpace(agent.Provider), strings.TrimSpace(agent.Model)
		if provider == "" {
			provider = agent.Name
		}
		if model == "" {
			model = agent.Name
		}
		candidate := groupCandidate{agent: agent, provider: provider, model: model,
			active: r.limiter(agent).occupancy(), max: agentMaxParallel(agent)}
		candidates = append(candidates, candidate)
		bySelector[agent.Name] = candidate
		if strings.TrimSpace(agent.Model) != "" {
			bySelector[agent.Model] = candidate
		}
	}
	models := make([]string, len(seats))
	providerUses, modelUses, agentUses := map[string]int{}, map[string]int{}, map[string]int{}
	assign := func(index int, candidate groupCandidate) error {
		if candidate.max > 0 && candidate.active+agentUses[candidate.agent.Name] >= candidate.max {
			return fmt.Errorf("%w: delegate group exceeds %s max_parallel=%d",
				delegatecontract.ErrDelegateCapacity, candidate.agent.Name, candidate.max)
		}
		models[index] = candidate.agent.Name
		providerUses[candidate.provider]++
		modelUses[candidate.model]++
		agentUses[candidate.agent.Name]++
		return nil
	}
	for i, seat := range seats {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if selectorUnbound(seat.Model) {
			continue
		}
		candidate, exists := bySelector[seat.Model]
		if !exists || !eligible(candidate.agent, seat.Role, seat.Persona) {
			return nil, fmt.Errorf("delegate model %q is not eligible for %s/%s", seat.Model, seat.Role, seat.Persona)
		}
		if err := assign(i, candidate); err != nil {
			return nil, err
		}
	}
	for i, seat := range seats {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if models[i] != "" {
			continue
		}
		best := -1
		hasEligible := false
		for j, candidate := range candidates {
			if !eligible(candidate.agent, seat.Role, seat.Persona) {
				continue
			}
			hasEligible = true
			if candidate.max > 0 && candidate.active+agentUses[candidate.agent.Name] >= candidate.max {
				continue
			}
			if best < 0 || groupCandidateLess(candidate, candidates[best],
				providerUses, modelUses, agentUses) {
				best = j
			}
		}
		if best < 0 {
			if hasEligible {
				// Eligible candidates filtered out only by occupancy are retryable load.
				return nil, fmt.Errorf("%w: delegate group cannot fill seat %d (%s/%s) within enabled capacity",
					delegatecontract.ErrDelegateCapacity, i+1, seat.Role, seat.Persona)
			}
			// No eligible healthy candidate is reachability, deliberately not capacity.
			return nil, fmt.Errorf("delegate group has no eligible healthy backend for seat %d (%s/%s)",
				i+1, seat.Role, seat.Persona)
		}
		if err := assign(i, candidates[best]); err != nil {
			return nil, err
		}
	}
	return models, nil
}

func selectAgent(registry agentRegistry, model, role, persona string) (agentEntry, error) {
	name := strings.TrimSpace(model)
	explicit := name != "" && name != "$random"
	if name == "" || name == "$random" {
		name = strings.TrimSpace(registry.DefaultAgent)
	}
	if name != "" {
		for _, a := range registry.roster() {
			if enabled(a) && available(a) && !a.PrimaryOnly && strings.TrimSpace(a.CLICmd) != "" && eligible(a, role, persona) &&
				(a.Name == name || a.Model == name) {
				return a, nil
			}
		}
		if explicit {
			return agentEntry{}, fmt.Errorf("delegate model %q is not an enabled CLI agent", name)
		}
	}
	for _, a := range registry.roster() {
		if enabled(a) && available(a) && !a.PrimaryOnly && strings.TrimSpace(a.CLICmd) != "" &&
			eligible(a, role, persona) {
			return a, nil
		}
	}
	return agentEntry{}, errors.New("no enabled delegate CLI is configured")
}

func splitCommand(command string) ([]string, error) {
	var out []string
	var word strings.Builder
	var quote rune
	escaped := false
	flush := func() {
		if word.Len() > 0 {
			out = append(out, word.String())
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
		case ' ', '\t', '\n':
			flush()
		case ';', '|', '&', '<', '>', '`':
			return nil, errors.New("delegate CLI command contains a shell operator")
		default:
			word.WriteRune(ch)
		}
	}
	if escaped || quote != 0 {
		return nil, errors.New("delegate CLI command has an unterminated escape or quote")
	}
	flush()
	if len(out) == 0 {
		return nil, errors.New("delegate CLI command is empty")
	}
	return out, nil
}

func executorArgv(agent agentEntry, request delegatecontract.Invocation) ([]string, error) {
	argv, err := splitCommand(agent.CLICmd)
	if err != nil {
		return nil, err
	}
	kind := strings.ToLower(strings.TrimSpace(agent.CLIKind))
	switch kind {
	case "claude", "claude-code", "codex":
		return nil, errors.New("native SDK launch requires the canonical host execution owner")
	case "generic":
		// Explicit operator-declared external text transport. Tools=true permits
		// external work; it never attests host-governed model tool execution.
		if !RoleIsWrite(request.Role) {
			return nil, errors.New("generic delegate CLIs cannot prove read-only role isolation")
		}
		if !request.Tools {
			return nil, errors.New("generic delegate CLIs cannot guarantee a tools-disabled invocation")
		}
		if request.MaxTurns > 0 {
			return nil, errors.New("generic delegate CLIs cannot enforce a maximum turn count")
		}

	default:
		return nil, fmt.Errorf("unsupported delegate cli_kind %q", agent.CLIKind)
	}
	return argv, nil
}

type limitedBuffer struct {
	mu        sync.Mutex
	buffer    bytes.Buffer
	remaining int
	overflow  bool
}

func (b *limitedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	n := len(p)
	if b.remaining <= 0 {
		b.overflow = b.overflow || n > 0
		return n, nil
	}
	if len(p) > b.remaining {
		b.overflow = true
		p = p[:b.remaining]
	}
	_, _ = b.buffer.Write(p)
	b.remaining -= len(p)
	return n, nil
}

func (b *limitedBuffer) Overflow() bool { b.mu.Lock(); defer b.mu.Unlock(); return b.overflow }

func (b *limitedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buffer.String()
}

func (b *limitedBuffer) BytesCopy() []byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	return append([]byte(nil), b.buffer.Bytes()...)
}

// turnMonitor enforces MaxTurns from observable JSONL events. Claude emits one
// assistant event per model turn. Codex exec exposes one initial model turn and
// completed non-reasoning/non-final items for tool work; counting each such
// item as another turn is conservative when tools run in parallel, so it may
// stop early but can never permit more model continuations than the cap.
func boundedTerminationDetail(detail string) string {
	detail = strings.ToValidUTF8(delegatecontract.SafeDiagnostic(strings.TrimSpace(detail)), "�")
	if len(detail) <= delegatecontract.TerminationDetailMax {
		return detail
	}
	end := delegatecontract.TerminationDetailMax
	for end > 0 && !utf8.ValidString(detail[:end]) {
		end--
	}
	return detail[:end]
}

func terminationDiagnostic(kind string, request delegatecontract.Invocation, elapsed time.Duration, detail string) *delegatecontract.TerminationDiagnostic {
	return &delegatecontract.TerminationDiagnostic{Kind: kind, ExecutionTimeoutMS: request.ExecutionTimeoutMS, ElapsedMS: elapsed.Milliseconds(), Detail: boundedTerminationDetail(detail)}
}

// RunWatchdog handles the private re-exec mode used by executorCommand. Every
// delegates process entry point calls this before parsing normal arguments.
func RunWatchdog(args []string) (bool, int) {
	if len(args) != 2 || args[1] != watchdogArgument {
		return false, 0
	}
	var argv []string
	if json.Unmarshal([]byte(os.Getenv(watchdogArgvEnv)), &argv) != nil || len(argv) == 0 {
		return true, 125
	}
	control := os.NewFile(3, "delegate-producer-lifecycle")
	if control == nil {
		return true, 125
	}
	defer control.Close()
	syscall.CloseOnExec(int(control.Fd()))
	return true, runWatchdog(control, argv, os.Stdin, os.Stdout, os.Stderr)
}

func runWatchdog(control io.Reader, argv []string, stdin io.Reader,
	stdout, stderr io.Writer) int {
	// Reparent grandchildren here so killing the process group cannot leave
	// zombies behind after the CLI leader exits.
	if err := unix.Prctl(unix.PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0); err != nil {
		_, _ = fmt.Fprintf(stderr, "configure delegate watchdog: %v\n", err)
		return 125
	}
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = stdin, stdout, stderr
	// The CLI leads a fresh group. Pdeathsig covers an unexpected watchdog
	// failure; the producer pipe below covers module death and kills the group.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
	if err := cmd.Start(); err != nil {
		_, _ = fmt.Fprintf(stderr, "start delegate CLI: %v\n", err)
		return 125
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	producerGone := make(chan struct{})
	go func() {
		_, _ = io.Copy(io.Discard, control)
		close(producerGone)
	}()
	select {
	case err := <-done:
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		reapWatchdogChildren()
		if err == nil {
			return 0
		}
		var exit *exec.ExitError
		if errors.As(err, &exit) && exit.ExitCode() >= 0 {
			return exit.ExitCode()
		}
		return 125
	case <-producerGone:
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		<-done
		reapWatchdogChildren()
		return 125
	}
}

func reapWatchdogChildren() {
	for {
		var status syscall.WaitStatus
		if _, err := syscall.Wait4(-1, &status, 0, nil); errors.Is(err, syscall.ECHILD) {
			return
		} else if err != nil && !errors.Is(err, syscall.EINTR) {
			return
		}
	}
}

func executorCommand(ctx context.Context, argv []string) (*exec.Cmd, func(), error) {
	executable, err := os.Executable()
	if err != nil {
		return nil, nil, err
	}
	encoded, err := json.Marshal(argv)
	if err != nil {
		return nil, nil, err
	}
	controlRead, controlWrite, err := os.Pipe()
	if err != nil {
		return nil, nil, err
	}
	cmd := exec.CommandContext(ctx, executable, watchdogArgument)
	cmd.Env = append(os.Environ(), watchdogArgvEnv+"="+string(encoded))
	cmd.ExtraFiles = []*os.File{controlRead}
	var closeOnce sync.Once
	closeControl := func() {
		closeOnce.Do(func() {
			_ = controlWrite.Close()
			_ = controlRead.Close()
		})
	}
	cmd.Cancel = func() error {
		closeControl()
		return nil
	}
	cmd.WaitDelay = 5 * time.Second
	return cmd, closeControl, nil
}

func (r *RegistryExecutor) Execute(ctx context.Context, request delegatecontract.Invocation) delegatecontract.InvocationResult {
	result := delegatecontract.InvocationResult{Version: delegatecontract.WireVersion, Status: "failed"}
	if ctx == nil {
		ctx = context.Background()
	}
	registry, err := r.load()
	if err != nil {
		result.Error = err.Error()
		return result
	}
	agent, err := selectAgent(registry, request.Model, request.Role, request.Persona)
	if err != nil {
		result.Error = err.Error()
		return result
	}
	result.Agent = agent.Name
	release, err := r.limiter(agent).acquire(ctx)
	if err != nil {
		result.Error = err.Error()
		return result
	}
	defer release()
	if RoleIsWrite(request.Role) && strings.TrimSpace(request.Workdir) == "" {
		result.Error = "write-capable delegate requires an isolated worktree"
		return result
	}
	if request.Workdir != "" {
		if !filepath.IsAbs(request.Workdir) {
			result.Error = "delegate workdir must be absolute"
			return result
		}
		if _, err := os.Lstat(filepath.Join(request.Workdir, ".git")); err != nil {
			result.Error = "delegate workdir is not a git checkout"
			return result
		}
	}
	kind := strings.ToLower(strings.TrimSpace(agent.CLIKind))
	switch kind {
	case "claude", "claude-code", "codex":
		return r.executeHost(ctx, request, agent)
	}
	argv, err := executorArgv(agent, request)
	if err != nil {
		result.Error = err.Error()
		return result
	}
	prompt, err := delegatecontract.CLIText(request.Input)
	if err != nil {
		result.Error = err.Error()
		return result
	}

	runCtx, runCancel := context.WithCancel(ctx)
	defer runCancel()
	cmd, closeCommand, err := executorCommand(runCtx, argv)
	if err != nil {
		result.Error = err.Error()
		return result
	}
	defer closeCommand()
	if request.Workdir != "" {
		cmd.Dir = request.Workdir
	}
	cmd.Stdin = strings.NewReader(prompt)
	output := &limitedBuffer{remaining: maxExecutorOutput}
	diagnostics := &limitedBuffer{remaining: maxExecutorOutput}
	cmd.Stdout, cmd.Stderr = output, diagnostics
	started := time.Now()
	if err := cmd.Run(); err != nil {
		elapsed := time.Since(started)
		if errors.Is(ctx.Err(), context.DeadlineExceeded) {
			result.Error = delegatecontract.ErrDelegateExecutionDeadline.Error()
			if detail := boundedTerminationDetail(output.String()); detail != "" {
				result.Error += ": " + detail
			}
			result.Termination = terminationDiagnostic(delegatecontract.TerminationExecutionDeadline,
				request, elapsed, output.String())
			return result
		}
		if errors.Is(ctx.Err(), context.Canceled) {
			result.Error = context.Canceled.Error()
			result.Termination = terminationDiagnostic(delegatecontract.TerminationCancelled,
				request, elapsed, output.String())
			return result
		}
		detail := strings.TrimSpace(diagnostics.String())
		if detail == "" {
			detail = strings.TrimSpace(output.String())
		}
		if detail == "" {
			detail = err.Error()
		}
		result.Error = delegatecontract.SafeDiagnostic(detail)
		return result
	}
	if output.Overflow() {
		result.Error = "external text adapter exceeded the complete-output limit"
		return result
	}
	response, history, parseErr := externalTextObservation(output.BytesCopy())
	result.Output, result.History = response, history
	if parseErr != nil {
		result.Error = parseErr.Error()
		return result
	}
	result.Status = "done"
	if err := delegatecontract.AdmitResult(&result); err != nil {
		result.Status, result.Error = "failed", err.Error()
		result.Response = ""
	}
	return result
}

// cancelledContext bridges the bus runtime's cancellation bit into os/exec.
func cancelledContext(invocationCancelled func() bool) (context.Context, context.CancelFunc) {
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		ticker := time.NewTicker(50 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if invocationCancelled() {
					cancel()
					return
				}
			}
		}
	}()
	return ctx, cancel
}

var _ io.Writer = (*limitedBuffer)(nil)
