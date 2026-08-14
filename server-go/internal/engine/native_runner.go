package engine

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

type Verifier interface {
	Verify(context.Context, string) error
}

type CommandVerifier struct {
	Command  []string
	LockFile string
}

const defaultCommandVerifyLock = "aimee-wfe-command-verify.lock"

// ErrGitIdentityMissing is a permanent deployment prerequisite, not a transient
// runner outage. The engine gives it a non-auto-resumed park reason so a missing
// install-time identity cannot launch a new implementation delegate every five
// seconds while no commit can succeed.
var ErrGitIdentityMissing = errors.New("git identity is not configured")

// gitIdentityArgs returns an explicit environment-provided identity for local
// development and tests. Production deliberately scrubs these values from the
// long-lived Go process and resolves the sealed install identity just in time
// through GitIdentityProvider instead.
//
// aimee has no ambient identity to fall back on: the server's git paths point
// GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM at /dev/null, so a commit carries the
// identity aimee supplies or it has no author and git refuses it.
//
// The WFE used to supply an aimee-wfe persona instead. That is worse than
// untidy: GitHub adds a Co-authored-by trailer to the squash of any PR whose
// commits carry two distinct authors, and the standing directive forbids those.
// One author, no trailer.
//
// Empty means the deployment configured none; callers refuse rather than commit
// anonymously.
func gitIdentityArgs() []string {
	name, email := os.Getenv("AIMEE_GIT_AUTHOR_NAME"), os.Getenv("AIMEE_GIT_AUTHOR_EMAIL")
	if strings.TrimSpace(name) == "" || strings.TrimSpace(email) == "" {
		return nil
	}
	return []string{"-c", "user.name=" + name, "-c", "user.email=" + email}
}

func defaultVerifyCommand() []string {
	// `git verify` is a key=value-style infrastructure command. Its machine
	// format is selected with format=json; `--json` is parsed as a value-taking
	// long flag and exits 2 before verification runs.
	return []string{"aimee", "git", "verify", "format=json"}
}

func (v CommandVerifier) Verify(ctx context.Context, workdir string) error {
	release, err := v.acquire(ctx)
	if err != nil {
		return err
	}
	defer release()

	command := v.Command
	if len(command) == 0 {
		command = defaultVerifyCommand()
	}
	cmd := exec.CommandContext(ctx, command[0], command[1:]...)
	cmd.Dir = workdir
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("verify failed: %s", strings.TrimSpace(string(output)))
	}
	return nil
}

// acquire serializes repository-wide verification across workflow workers and
// server processes on the same host. The C unit suite still contains tests that
// bind process-global resources; independently isolated worktrees and HOME
// directories are not enough to make several complete suites safe in parallel.
// A file lock also releases automatically if the server crashes.
func (v CommandVerifier) acquire(ctx context.Context) (func(), error) {
	lockPath := strings.TrimSpace(v.LockFile)
	if lockPath == "" {
		lockPath = filepath.Join(os.TempDir(), defaultCommandVerifyLock)
	}
	lock, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open verifier lock: %w", err)
	}
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		err = syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
		if err == nil {
			return func() {
				_ = syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
				_ = lock.Close()
			}, nil
		}
		if !errors.Is(err, syscall.EWOULDBLOCK) && !errors.Is(err, syscall.EAGAIN) {
			_ = lock.Close()
			return nil, fmt.Errorf("lock verifier: %w", err)
		}
		select {
		case <-ctx.Done():
			_ = lock.Close()
			return nil, ctx.Err()
		case <-ticker.C:
		}
	}
}

type NativeRunner struct {
	db        *db1.Store
	worktrees *WorktreeManager
	agents    AgentClient
	verifier  Verifier
	artifacts *wfe.ArtifactStore
	workflows *wfe.Registry
	forge     Forge
	// reviews convenes a roundtable. The runner does not host a panel: the
	// module does, over the bus, so this is the whole of the runner's coupling
	// to reviewing.
	reviews RoundtableReviewer
}

// RoundtableReviewer convenes one review. Narrow on purpose: the runner depends
// on the capability, not on whatever transport reaches it.
type RoundtableReviewer interface {
	Review(context.Context, roundtablecfg.ReviewRequest) (roundtablecfg.RunResult, error)
}

func (r *NativeRunner) SetRoundtableReviewer(reviewer RoundtableReviewer) { r.reviews = reviewer }

const (
	roundtableDelegateRole        = "review"
	roundtableDelegateMaxTurnsCap = 24
	delegateDeadlineGraceReserve  = 5 * time.Second
	delegateWriteVerifyReserve    = 5 * time.Minute
	// Keep the Go admission boundary aligned with AGENT_LOOP_MIN_CALL_MS in
	// agent_types.h. Dispatching a write delegate with less than one viable
	// model-call window only creates a zero-call failed job before the C runtime
	// reports that its tool-loop budget is exhausted.
	delegateWriteMinRunBudget = time.Minute
)

// DelegateLimitError names both time bounds that could have stopped a delegate,
// and how long it actually ran. Two independent limits bound one dispatch -- the
// stage wall cap and the delegate's own tool-loop budget -- and a bare "context
// deadline exceeded" says neither which one fired nor what the other one was, so
// a conflicting pair could not be diagnosed from the event log at all.
//
// The C runtime already reports both numbers when its own budget ends the loop
// ("tool loop budget exhausted (elapsed=... effective=... configured=...
// stage_remaining_cap=...)", src/posix/agent_runtime.c). This carries the
// equivalent for the other direction, where the stage deadline fires first.
type DelegateLimitError struct {
	Err error
	// StageWallRemaining is the stage wall budget this dispatch was given.
	StageWallRemaining time.Duration
	// ToolLoopCap is the tool-loop budget actually handed to the delegate, after
	// applyDelegateDeadlineCap bounded it by the stage's remaining wall.
	ToolLoopCap time.Duration
	// Elapsed is how long the dispatch ran before it failed.
	Elapsed time.Duration
}

func (e *DelegateLimitError) Error() string {
	return fmt.Sprintf("%s (stage_wall_remaining=%s delegate_tool_loop_cap=%s elapsed=%s)",
		e.Err, boundOrUnset(e.StageWallRemaining), boundOrUnset(e.ToolLoopCap),
		e.Elapsed.Round(time.Millisecond))
}

// Exactly zero means the bound was never set — no deadline on the context, or no
// tool-loop cap applied — which is a different fact from a bound of zero length.
// Printing "0s" for it invites the misreading this error exists to prevent: that
// the limit was reached instantly.
//
// A NEGATIVE value is a third, distinct fact. StageWallRemaining comes from
// time.Until(deadline), which goes negative once the deadline has passed and is
// not clamped, so a non-positive test would report the one case where the limit
// provably WAS reached as though no limit existed — the same inversion, pointing
// the other way. Negatives are rendered as themselves.
func boundOrUnset(d time.Duration) string {
	if d == 0 {
		return "unset"
	}
	return d.Round(time.Millisecond).String()
}

func (e *DelegateLimitError) Unwrap() error { return e.Err }

func (r *NativeRunner) delegate(ctx context.Context, step StepRequest, request DelegateRequest) (DelegateResult, error) {
	if err := applyDelegateDeadlineCap(ctx, &request); err != nil {
		return DelegateResult{}, err
	}
	request.WorkItemID = step.WorkItem.ID
	request.Stage = step.Node.ID
	request.ExecutionVersion = step.WorkItem.UpdatedAt
	request.MaxCostUSD = step.CostLimitUSD
	request.ReplayOnly = step.ReplayOnly
	stageWallRemaining := time.Duration(0)
	if deadline, ok := ctx.Deadline(); ok {
		stageWallRemaining = time.Until(deadline)
	}
	started := time.Now()
	result, err := r.agents.Delegate(ctx, request)
	// Only the stage-deadline direction is annotated. When the delegate's own
	// budget ends the loop the C runtime already names both limits, and wrapping
	// every unrelated dispatch failure would bury its cause behind timings that
	// had nothing to do with it.
	if err != nil && errors.Is(err, context.DeadlineExceeded) {
		return result, &DelegateLimitError{
			Err:                err,
			StageWallRemaining: stageWallRemaining,
			ToolLoopCap:        time.Duration(request.ToolLoopTimeoutMSCap) * time.Millisecond,
			Elapsed:            time.Since(started),
		}
	}
	return result, err
}

func (r *NativeRunner) delegateGroup(ctx context.Context, step StepRequest, requests []DelegateRequest) []DelegateGroupResult {
	if len(requests) == 0 {
		return nil
	}
	for i := range requests {
		if err := applyDelegateDeadlineCap(ctx, &requests[i]); err != nil {
			out := make([]DelegateGroupResult, len(requests))
			for j := range out {
				out[j].Err = err
			}
			return out
		}
		requests[i].WorkItemID = step.WorkItem.ID
		requests[i].Stage = step.Node.ID
		requests[i].ExecutionVersion = step.WorkItem.UpdatedAt
		requests[i].ReplayOnly = step.ReplayOnly
		if step.CostLimitUSD > 0 {
			// Group calls execute concurrently, so their individual ceilings must
			// sum to no more than the step reservation.
			requests[i].MaxCostUSD = step.CostLimitUSD / float64(len(requests))
		}
	}
	if group, ok := r.agents.(DelegateGroupClient); ok {
		return group.DelegateGroup(ctx, requests)
	}
	// Roundtable never reconstructs grouped delegation or participant identity.
	// A resource plane without the generic group contract is unavailable to it.
	out := make([]DelegateGroupResult, len(requests))
	for i := range requests {
		out[i].Err = errors.New("delegate service does not support grouped delegation")
	}
	return out
}

// applyDelegateDeadlineCap converts the enclosing stage deadline into a
// resource-plane tool-loop cap. Write delegates leave enough time for the
// mandatory repository verifier; read-only delegates only need cancellation
// and lifecycle-transition slack. The cap can only reduce the agent's own
// configured loop budget.
func applyDelegateDeadlineCap(ctx context.Context, request *DelegateRequest) error {
	deadline, ok := ctx.Deadline()
	if !ok {
		return nil
	}
	remaining := time.Until(deadline)
	reserve := remaining / 20
	if reserve > delegateDeadlineGraceReserve {
		reserve = delegateDeadlineGraceReserve
	}
	if request.Role == "code" && request.Tools {
		reserve = delegateWriteVerifyReserve
		if remaining < reserve+delegateWriteMinRunBudget {
			return fmt.Errorf("delegate stage wall budget exhausted: remaining=%s reserve=%s minimum_run=%s: %w",
				remaining.Round(time.Millisecond), reserve, delegateWriteMinRunBudget,
				context.DeadlineExceeded)
		}
	}
	capMillis := (remaining - reserve).Milliseconds()
	maxInt := int64(^uint(0) >> 1)
	if capMillis > maxInt {
		capMillis = maxInt
	}
	if capMillis < 1 {
		capMillis = 1
	}
	capValue := int(capMillis)
	if request.ToolLoopTimeoutMSCap <= 0 || request.ToolLoopTimeoutMSCap > capValue {
		request.ToolLoopTimeoutMSCap = capValue
	}
	return nil
}

func NewNativeRunner(db *db1.Store, worktrees *WorktreeManager, agents AgentClient, verifier Verifier, artifacts *wfe.ArtifactStore, workflows *wfe.Registry, forge Forge) (*NativeRunner, error) {
	if db == nil || worktrees == nil || agents == nil || artifacts == nil || workflows == nil {
		return nil, errors.New("DB1, worktrees, agent client, artifacts, and workflow registry are required")
	}
	if verifier == nil {
		verifier = CommandVerifier{}
	}
	if forge == nil {
		forge = unavailableForge{}
	}
	return &NativeRunner{db: db, worktrees: worktrees, agents: agents, verifier: verifier, artifacts: artifacts, workflows: workflows, forge: forge}, nil
}

func (r *NativeRunner) Run(ctx context.Context, req StepRequest) (StepResult, error) {
	if req.WorkItem.ParentID != "" {
		result, blocked, err := r.packetDependencyGate(ctx, req.WorkItem)
		if err != nil {
			return StepResult{}, err
		}
		if blocked {
			return result, nil
		}
	}
	switch req.Node.Block {
	case "trigger.watch-dir", "author.proposal":
		return StepResult{Status: StepAdvanced, ArtifactType: "proposal", Artifact: req.Proposal}, nil
	case "author.plan":
		return r.author(ctx, req, "plan")
	case "understand":
		return r.structured(ctx, req, "intent")
	case "split":
		return r.structured(ctx, req, "packets")
	case "branch.open":
		return r.branchOpen(ctx, req)
	case "implement":
		return r.mutate(ctx, req, false)
	case "document":
		return r.mutate(ctx, req, true)
	case "freeze":
		return r.freeze(ctx, req)
	case "gate.roundtable":
		return r.roundtable(ctx, req)
	case "review":
		return r.review(ctx, req)
	case "gate.human":
		return StepResult{Status: StepPending, PauseReason: "human_gate", Detail: paramString(req.Node, "policy", "approval required")}, nil
	case "gate.deliver":
		return StepResult{Status: StepAdvanced, ArtifactType: "none", Artifact: "delivered"}, nil
	case "check.mergeable":
		return r.checkMergeable(ctx, req)
	case "foreach.workflow":
		return r.foreach(ctx, req)
	case "pr.open":
		return r.prOpen(ctx, req)
	case "gate.ci":
		return r.gateCI(ctx, req)
	case "merge":
		return r.merge(ctx, req)
	case "source.archive":
		return r.archive(ctx, req)
	default:
		block := req.Block
		if block.Name == "" || !block.Custom {
			return StepResult{}, fmt.Errorf("native Go runner does not implement block %q", req.Node.Block)
		}
		return r.custom(ctx, req, block)
	}
}

type workflowPacket struct {
	PacketID     string   `json:"packet_id"`
	Dependencies []string `json:"dependencies"`
}

// packetDependencyGate keeps a generated slice at its current stage until every
// dependency in the approved packet plan has actually reached accepted. Packet
// order and the per-workflow concurrency limit are scheduling policy, not a
// dependency contract: a parked predecessor must never make the next slice
// runnable merely because it released an execution slot.
func (r *NativeRunner) packetDependencyGate(ctx context.Context, item db1.WorkItem) (StepResult, bool, error) {
	content, err := r.artifacts.Proposal(item.ID)
	if err != nil {
		return StepResult{}, false, fmt.Errorf("load slice packet: %w", err)
	}
	var packet workflowPacket
	if err := json.Unmarshal(content, &packet); err != nil || strings.TrimSpace(packet.PacketID) == "" {
		if err == nil {
			err = errors.New("packet_id is required")
		}
		return StepResult{}, false, fmt.Errorf("decode slice packet: %w", err)
	}
	if len(packet.Dependencies) == 0 {
		return StepResult{}, false, nil
	}

	lastDot := strings.LastIndexByte(item.ID, '.')
	if lastDot < 0 {
		return StepResult{}, false, errors.New("slice work-item id has no packet generation")
	}
	generationPrefix := item.ID[:lastDot+1]
	siblings, err := r.db.Children(ctx, item.ParentID)
	if err != nil {
		return StepResult{}, false, fmt.Errorf("load slice siblings: %w", err)
	}
	byPacketID := make(map[string]db1.WorkItem, len(siblings))
	for _, sibling := range siblings {
		if !strings.HasPrefix(sibling.ID, generationPrefix) {
			continue
		}
		siblingContent, readErr := r.artifacts.Proposal(sibling.ID)
		if readErr != nil {
			return StepResult{}, false, fmt.Errorf("load sibling packet %s: %w", sibling.ID, readErr)
		}
		var siblingPacket workflowPacket
		if err := json.Unmarshal(siblingContent, &siblingPacket); err != nil || strings.TrimSpace(siblingPacket.PacketID) == "" {
			if err == nil {
				err = errors.New("packet_id is required")
			}
			return StepResult{}, false, fmt.Errorf("decode sibling packet %s: %w", sibling.ID, err)
		}
		if _, exists := byPacketID[siblingPacket.PacketID]; exists {
			return StepResult{}, false, fmt.Errorf("duplicate packet_id %s in slice generation", siblingPacket.PacketID)
		}
		byPacketID[siblingPacket.PacketID] = sibling
	}

	for _, dependencyID := range packet.Dependencies {
		dependency, ok := byPacketID[dependencyID]
		if !ok {
			return StepResult{Status: StepFailed, Detail: "packet dependency " + dependencyID + " is unavailable"}, true, nil
		}
		switch dependency.State {
		case "accepted":
			continue
		case "rejected", "stopped", "abandoned":
			return StepResult{Status: StepFailed, Detail: fmt.Sprintf("packet dependency %s ended %s", dependencyID, dependency.State)}, true, nil
		default:
			detail := fmt.Sprintf("waiting for packet dependency %s (%s)", dependencyID, dependency.State)
			if dependency.PauseReason != "" {
				detail += ": " + dependency.PauseReason
			}
			return StepResult{Status: StepPending, PauseReason: "dependency_pending", Detail: detail}, true, nil
		}
	}
	return StepResult{}, false, nil
}

func (r *NativeRunner) custom(ctx context.Context, req StepRequest, block wfe.BlockDefinition) (StepResult, error) {
	workdir := req.WorkItem.Repo
	branch := ""
	if block.Produces == "branch" || block.Consumes == "branch" {
		var err error
		workdir, branch, err = r.worktrees.Ensure(ctx, req.WorkItem, req.WorkItem.ParentID == "")
		if err != nil {
			return StepResult{}, err
		}
	}
	// Proposal is the historical wire name for the immutable workflow-entry request.
	prompt := block.Prompt + "\n\nORIGINAL REQUEST:\n" + req.Proposal
	for name, input := range req.Inputs {
		prompt += "\n\nINPUT " + name + " (" + input.Type + "):\n" + string(input.Content)
	}
	if block.Executor == "command" {
		if len(block.Command) == 0 {
			return StepResult{}, errors.New("custom command has no argv")
		}
		commandCtx := ctx
		cancel := func() {}
		if block.CommandTimeoutMS > 0 {
			commandCtx, cancel = context.WithTimeout(ctx, time.Duration(block.CommandTimeoutMS)*time.Millisecond)
		}
		defer cancel()
		cmd := exec.CommandContext(commandCtx, block.Command[0], block.Command[1:]...)
		cmd.Dir = workdir
		cmd.Stdin = strings.NewReader(prompt)
		output, err := cmd.CombinedOutput()
		if err != nil {
			if req.Node.OnFail == "" {
				return StepResult{Status: StepFailed, Detail: strings.TrimSpace(string(output))}, nil
			}
			return StepResult{Status: StepChanges, Detail: strings.TrimSpace(string(output))}, nil
		}
		if block.Produces == "branch" {
			if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
				return StepResult{}, err
			}
			if err := r.commitChanges(ctx, workdir, req.Node.ID); err != nil {
				return StepResult{}, err
			}
			head, err := gitText(ctx, workdir, "rev-parse", "HEAD")
			if err != nil {
				return StepResult{}, err
			}
			return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: head}, nil
		}
		return StepResult{Status: StepAdvanced, ArtifactType: block.Produces, Artifact: string(output)}, nil
	}
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "code", Persona: firstNonempty(paramString(req.Node, "persona", ""), block.Persona), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: workdir, Tools: true})
	if err != nil {
		return StepResult{}, err
	}
	if block.Produces == "branch" {
		if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
			return StepResult{}, err
		}
		if err := r.commitChanges(ctx, workdir, req.Node.ID); err != nil {
			return StepResult{}, err
		}
		head, err := gitText(ctx, workdir, "rev-parse", "HEAD")
		if err != nil {
			return StepResult{}, err
		}
		return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: head, CostUSD: result.CostUSD, CostUnknown: result.CostUnknown}, nil
	}
	return StepResult{Status: StepAdvanced, ArtifactType: block.Produces, Artifact: result.Response, CostUSD: result.CostUSD, CostUnknown: result.CostUnknown}, nil
}

func (r *NativeRunner) author(ctx context.Context, req StepRequest, kind string) (StepResult, error) {
	proposal, ok := req.Inputs["proposal"]
	if !ok {
		return StepResult{}, errors.New("author.plan missing proposal input")
	}
	// The proposal input is the immutable workflow-entry request; only its schema name is historical.
	prompt := "Author a complete implementation plan for the original request below. Return only the plan; do not truncate it. " +
		"Complete means every part of the request is covered, not that the plan is large. Plan the smallest work that satisfies the request as written: " +
		"do not add deliverables, mechanisms, file formats, flags, or migrations the request did not ask for, and do not generalize a specific ask into a framework. " +
		"Work the request did not ask for but that you judge genuinely necessary is technical debt. Taking on documented technical debt is completely acceptable; the requirement is that it is written down. " +
		"Name it under a Technical debt, Deferred follow-up, or Non-goals heading and do not plan it — that is a correct and expected outcome, not a failure to plan. " +
		"Deferring it means planning none of it, including its groundwork: do not plan a store, fixture, format, or hook whose only purpose is to enable work this same plan defers. " +
		"What is not acceptable is leaving it undocumented: debt you neither plan nor record is a gap that silently ships.\n\nORIGINAL REQUEST:\n" + string(proposal.Content)
	if req.Feedback != nil {
		encoded, _ := json.Marshal(req.Feedback)
		prompt += "\n\nPRIOR REVIEW FEEDBACK TO RESOLVE:\n" + string(encoded)
	}
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "draft", Persona: paramString(req.Node, "persona", "architect"), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: req.WorkItem.Repo})
	if err != nil {
		return StepResult{}, err
	}
	if strings.TrimSpace(result.Response) == "" {
		return StepResult{Status: StepChanges, Detail: "planner returned an empty artifact", CostUSD: result.CostUSD, CostUnknown: result.CostUnknown}, nil
	}
	return StepResult{Status: StepAdvanced, ArtifactType: kind, Artifact: result.Response, CostUSD: result.CostUSD, CostUnknown: result.CostUnknown}, nil
}

func (r *NativeRunner) structured(ctx context.Context, req StepRequest, kind string) (StepResult, error) {
	var prompt string
	if kind == "intent" {
		prompt = "Scope the engineering task below. Return only JSON shaped {\"schema_version\":1,\"status\":\"unconfirmed\",\"summary\":\"...\",\"rationale\":\"...\",\"acceptance_criteria\":[\"...\"]}. Describe the task, never the bookkeeping record.\n\nTASK:\n" + req.Proposal
	} else {
		source := inputText(req, "plan")
		if source == "" {
			source = inputText(req, "intent")
		}
		if source == "" {
			return StepResult{}, errors.New("split requires an in.plan or in.intent artifact binding")
		}
		if requestRequiresSingleSlice(req.Proposal) {
			title, err := pullRequestTitle(req.Proposal)
			if err != nil {
				return StepResult{}, fmt.Errorf("single-slice request title: %w", err)
			}
			content, err := json.Marshal(map[string]any{
				"schema_version": 1,
				"packets": []map[string]any{{
					"packet_id":     "p1",
					"summary":       title,
					"target_blocks": []string{"implement"},
					"dependencies":  []string{},
					"acceptance_criteria": []string{
						"Implement the complete approved plan as one reviewable change.",
						"Do not add deferred, post-adoption, or otherwise out-of-scope deliverables.",
					},
					"original_request": req.Proposal,
					"approved_plan":    source,
				}},
			})
			if err != nil {
				return StepResult{}, err
			}
			return StepResult{Status: StepAdvanced, ArtifactType: "plan", Artifact: string(content)}, nil
		}
		prompt = "Decompose the complete approved plan into the smallest independent implementation packets that preserve the ORIGINAL REQUEST exactly. " +
			"Return only JSON shaped {\"schema_version\":1,\"packets\":[{\"packet_id\":\"p1\",\"summary\":\"...\",\"target_blocks\":[\"implement\"],\"dependencies\":[],\"acceptance_criteria\":[\"...\"]}]}. " +
			"Only create packets for repository changes that can be completed in this workflow run. Do not create packets for post-adoption measurements, future observation windows, operational follow-up, proposal bookkeeping, or manual verification. " +
			"Tests and acceptance checks are criteria, not packets, unless the original request explicitly asks for a new reusable test artifact. Every packet must trace to an explicit requested deliverable; useful extra work is scope drift. " +
			"Each summary becomes a pull request title: make it a concise reviewer-facing outcome that says what changes, not a process instruction such as inspect, only if necessary, or minimally update. Do not omit requested implementation work or truncate content.\n\n" +
			"ORIGINAL REQUEST:\n" + req.Proposal + "\n\nAPPROVED PLAN:\n" + source
		if req.Feedback != nil {
			encoded, _ := json.Marshal(req.Feedback)
			prompt += "\n\nACCEPTANCE FEEDBACK THAT THE NEW PACKETS MUST RESOLVE:\n" + string(encoded)
		}
	}
	var cost float64
	costUnknown := false
	var result DelegateResult
	var content []byte
	var validationErr error
	for attempt := 1; attempt <= 3; attempt++ {
		result, validationErr = r.delegate(ctx, req, DelegateRequest{Role: "draft", Persona: paramString(req.Node, "persona", "architect"), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: req.WorkItem.Repo})
		if validationErr != nil {
			return StepResult{}, validationErr
		}
		cost += result.CostUSD
		costUnknown = costUnknown || result.CostUnknown
		content, validationErr = extractJSONObject(result.Response)
		if validationErr == nil {
			validationErr = validateStructured(kind, content)
		}
		if validationErr == nil {
			break
		}
		// A workflow-level retry previously repeated the identical request without
		// telling the delegate what was malformed. Keep the complete response and
		// exact validation error so the next synthesis can repair, not regenerate,
		// the artifact. The changed prompt is part of DelegateRequest and therefore
		// receives a distinct durable job key. Do not byte-truncate the artifact:
		// silent byte limits were the original non-convergence failure mode.
		prompt += "\n\nYOUR PREVIOUS RESPONSE WAS INVALID (" + validationErr.Error() + "). Repair it and return one complete JSON object only. Do not truncate or omit any field.\n\nCOMPLETE INVALID RESPONSE:\n" + result.Response
	}
	if validationErr != nil {
		return StepResult{Status: StepChanges, Detail: "structured response remained invalid after corrective synthesis: " + validationErr.Error(), CostUSD: cost, CostUnknown: costUnknown}, nil
	}
	typeName := "intent"
	if kind == "packets" {
		typeName = "plan"
	}
	return StepResult{Status: StepAdvanced, ArtifactType: typeName, Artifact: string(content), CostUSD: cost, CostUnknown: costUnknown}, nil
}

func requestRequiresSingleSlice(request string) bool {
	replacer := strings.NewReplacer("-", " ", "‑", " ", "–", " ", "—", " ", "_", " ")
	for _, raw := range strings.Split(strings.ReplaceAll(request, "\r\n", "\n"), "\n") {
		line := strings.ToLower(replacer.Replace(raw))
		line = strings.Join(strings.Fields(line), " ")
		if strings.Contains(line, "state:") && strings.Contains(line, "single slice") {
			return true
		}
	}
	return false
}

func (r *NativeRunner) branchOpen(ctx context.Context, req StepRequest) (StepResult, error) {
	workdir, branch, err := r.worktrees.Ensure(ctx, req.WorkItem, true)
	if err != nil {
		return StepResult{}, err
	}
	if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
		return StepResult{}, err
	}
	if err := r.forge.Push(ctx, req.WorkItem.Repo, workdir, branch); err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: wfe.Hash([]byte(branch))}, nil
}

// documentDelegatePrompt anchors documentation work to the same immutable
// request and exact branch diff that the acceptance gate reviewed. A branch
// name alone invites the delegate to mine unrelated history for undocumented
// changes and expand the final PR after acceptance.
func documentDelegatePrompt(ctx context.Context, req StepRequest, workdir string) (string, error) {
	acceptedDiff, err := frozenWorktreeDiff(ctx, req.WorkItem, workdir)
	if err != nil {
		return "", err
	}
	return "Document only the accepted implementation of the original request below. " +
		"Do not infer work from unrelated repository history or document pre-existing changes. " +
		"Update appropriate user or developer documentation and inline comments only when the " +
		"accepted implementation needs it; if its documentation is already complete, leave the " +
		"worktree unchanged.\n\nORIGINAL REQUEST:\n" + req.Proposal +
		"\n\nACCEPTED IMPLEMENTATION DIFF:\n" + acceptedDiff, nil
}

// The shared write-role guard reports a successful no-op as a partial result so
// ordinary implementation steps cannot silently advance without producing work.
// Documentation is different: its prompt explicitly requires an unchanged tree
// when the accepted implementation is already documented. Recognize only the
// guard's stable diagnostics; unrelated partial results remain failures.
func delegatePartialIsNoChange(response string) bool {
	return strings.Contains(response, "result treated as incomplete") &&
		(strings.Contains(response, "no owned files changed") ||
			strings.Contains(response, "no file changes detected"))
}

// implementationPartialIsSatisfiedNoChange distinguishes an explicit
// completion report from a delegate that merely failed to edit anything. The
// implementation prompt requires this exact claim when sibling/base work
// already satisfies the packet; mechanical verification still runs before the
// unchanged HEAD can advance. Other partial no-change results remain failures.
func implementationPartialIsSatisfiedNoChange(response string) bool {
	return delegatePartialIsNoChange(response) &&
		strings.Contains(strings.ToLower(response), "task already complete")
}

func retryDetailForPrompt(detail string) string {
	detail = strings.TrimSpace(safeDiagnostic(detail))
	const maxRunes = 24_000
	runes := []rune(detail)
	if len(runes) <= maxRunes {
		return detail
	}
	const headRunes = 8_000
	return string(runes[:headRunes]) + "\n...[retry diagnostic truncated]...\n" + string(runes[len(runes)-(maxRunes-headRunes):])
}

func implementationDelegatePrompt() string {
	return "Implement the complete approved task in this worktree, run the repository verification, fix failures, and leave the accepted changes in the worktree. " +
		"If the current branch already fully satisfies the task (including work merged by a sibling), leave the worktree unchanged and report that it is complete; do not manufacture cosmetic changes."
}

func (r *NativeRunner) mutate(ctx context.Context, req StepRequest, docs bool) (StepResult, error) {
	workdir, branch, err := r.worktrees.Ensure(ctx, req.WorkItem, req.WorkItem.ParentID == "")
	if err != nil {
		return StepResult{}, err
	}
	// A slice worktree is cut off aimee/feat/<parent> once, at creation. Sibling
	// slices merge into that feature branch afterwards, but the reused worktree is
	// never re-synced — so a dependent slice runs against a base missing its
	// prerequisites, produces no diff, and parks convergence_no_progress. Integrate
	// the feature branch here, at code-mutating entry (implement/document + its TDD
	// pre-step below), so this attempt sees siblings that have merged since. Not done
	// at freeze/pr/ci/merge — those must observe a stable diff.
	if req.WorkItem.ParentID != "" {
		parkReason, integErr := r.integrateFeatureBase(ctx, workdir, req.WorkItem.ParentID)
		if integErr != nil {
			return StepResult{}, integErr
		}
		if parkReason != "" {
			return StepResult{Status: StepPending, PauseReason: parkReason,
				Detail: "conflict merging aimee/feat/" + req.WorkItem.ParentID + " into slice"}, nil
		}
	}
	// A review gate can be resumed after a human repair commit. The
	// reviewed hash then remains on both the work item and feedback artifact,
	// while the clean exact frozen diff has changed. Send that new diff back through
	// freeze + roundtable rather than demanding that a delegate invent another
	// edit solely to satisfy its "owned files changed" contract. Implementation
	// repairs must also pass the same mechanical verifier used after a delegate.
	// This does not bypass review, and feedback left by an earlier gate cannot
	// trigger it.
	// The first pass after review may verify a clean committed repair without
	// asking a delegate to make a meaningless extra edit. Once that verifier has
	// failed, the engine supplies its diagnostic and a later pass must dispatch an
	// implementation delegate. RetryDetail remains present across an operator
	// retry-budget reset, unlike the numeric attempt counter.
	repairFastPath := docs || req.RetryDetail == ""
	if repairFastPath && req.Feedback != nil && req.WorkItem.ContentHash != "" &&
		req.WorkItem.ContentHash == req.Feedback.ArtifactHash {
		diff, diffErr := frozenWorktreeDiff(ctx, req.WorkItem, workdir)
		if diffErr != nil {
			return StepResult{}, diffErr
		}
		status, statusErr := gitText(ctx, workdir, "status", "--porcelain")
		if statusErr != nil {
			return StepResult{}, statusErr
		}
		if status == "" && strings.TrimSpace(diff) != "" &&
			wfe.Hash([]byte(diff)) != req.Feedback.ArtifactHash {
			if !docs {
				if err := r.verifier.Verify(ctx, workdir); err != nil {
					return StepResult{Status: StepChanges, Detail: err.Error()}, nil
				}
			}
			head, headErr := gitText(ctx, workdir, "rev-parse", "HEAD")
			if headErr != nil {
				return StepResult{}, headErr
			}
			detail := "reviewed worktree advanced; re-freezing exact repair"
			if !docs {
				detail = "reviewed worktree advanced; verified and re-freezing exact repair"
			}
			return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch,
				ContentHash: head, Detail: detail}, nil
		}
	}
	prompt := implementationDelegatePrompt()
	if docs {
		prompt, err = documentDelegatePrompt(ctx, req, workdir)
		if err != nil {
			return StepResult{}, err
		}
	}
	if task := paramString(req.Node, "task", ""); task != "" {
		prompt += "\n\nWORKFLOW STEP INSTRUCTIONS:\n" + task
	}
	for name, input := range req.Inputs {
		prompt += "\n\nINPUT " + name + " (" + input.Type + "):\n" + string(input.Content)
	}
	if req.Feedback != nil {
		encoded, _ := json.Marshal(req.Feedback)
		prompt += "\n\nREVIEW FEEDBACK TO RESOLVE:\n" + string(encoded)
	}
	if retryDetail := retryDetailForPrompt(req.RetryDetail); retryDetail != "" {
		prompt += "\n\nPREVIOUS ATTEMPT FAILURE TO FIX:\n" + retryDetail
	}
	var cost float64
	costUnknown := false
	if !docs && paramBool(req.Node, "tdd") {
		testPrompt := "Write the failing tests required by this task before implementation. Run them to confirm they fail for the intended reason, and leave the tests in the worktree.\n\n" + prompt
		// implement/document are native branch-producing blocks regardless of the
		// custom block registry's Produces metadata. This pre-step is committed here,
		// and the completed implementation is verified before the step advances.
		testResult, testErr := r.delegate(ctx, req, DelegateRequest{Role: "code", Persona: paramString(req.Node, "test_persona", "qa"), Delegate: paramString(req.Node, "test_delegate", ""), Prompt: testPrompt, Workdir: workdir, Tools: true, AcceptPartial: true})
		if testErr != nil {
			return StepResult{}, testErr
		}
		cost += testResult.CostUSD
		costUnknown = costUnknown || testResult.CostUnknown
		if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
			return StepResult{}, err
		}
		if err := r.commitChanges(ctx, workdir, req.Node.ID+" tests"); err != nil {
			return StepResult{}, err
		}
		prompt += "\n\nTDD: failing tests have already been authored in the worktree. Make them pass without weakening or deleting their assertions."
	}
	// acceptPartial is granted here on the contract that this block "is
	// independently committed and verified by the Go native runner". Record the
	// pre-delegate HEAD so that promise can actually be checked below.
	baseHead, baseHeadErr := gitText(ctx, workdir, "rev-parse", "HEAD")
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "code", Persona: paramString(req.Node, "persona", "engineer"), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: workdir, Tools: true, AcceptPartial: true})
	if err != nil {
		return StepResult{}, err
	}
	cost += result.CostUSD
	costUnknown = costUnknown || result.CostUnknown
	if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
		return StepResult{}, err
	}
	if err := r.commitChanges(ctx, workdir, req.Node.ID); err != nil {
		return StepResult{}, err
	}
	// A delegate that reported partial AND left no commit did not implement the
	// task -- it said so itself. Advancing turns that into an empty diff at freeze,
	// which reads as "the work is already in the base" and accepts the slice, so a
	// run can reach done=N with no commits, no artifact and no PR. Only the partial
	// case fails here: a completed delegate that legitimately had nothing to do is
	// still the freeze no-op path.
	if result.Partial && baseHeadErr == nil {
		head, headErr := gitText(ctx, workdir, "rev-parse", "HEAD")
		// baseHead is HEAD at the start of THIS attempt, so on a redispatch it
		// already contains whatever earlier attempts committed. A delegate that
		// correctly finds the work done then leaves no new commit and looks
		// identical to one that did nothing at all -- and the slice retried until
		// its wall cap, forever. Observed on wi_e51e37cf slice g0.0: two "wfe:
		// impl" commits carrying the whole change, and every redispatch reporting
		// "no owned files changed; result treated as incomplete".
		//
		// So only fail when the BRANCH carries no work either. Ask the branch, not
		// this attempt. Review correction is stricter: an older implementation commit
		// cannot excuse a partial retry when its current diff is still byte-identical
		// to the artifact carrying blocking findings. That exact failure advanced
		// wi_57186250 from impl to freeze without addressing its second review, then
		// immediately exhausted the roundtable convergence limit.
		// A document no-op is the requested outcome when the accepted diff is
		// already documented. Freeze the exact unchanged HEAD so doc_freeze and
		// doc_gate still review it; blocking review feedback overrides that exemption.
		satisfiedNoop := (docs && delegatePartialIsNoChange(result.Response)) ||
			(!docs && implementationPartialIsSatisfiedNoChange(result.Response))
		blockingReviewUnchanged := false
		if headErr == nil && head == baseHead && feedbackHasBlockingFinding(req.Feedback) &&
			req.Feedback.ArtifactHash != "" {
			diff, diffErr := frozenWorktreeDiff(ctx, req.WorkItem, workdir)
			if diffErr != nil {
				return StepResult{}, diffErr
			}
			blockingReviewUnchanged = wfe.Hash([]byte(diff)) == req.Feedback.ArtifactHash
		}
		if headErr == nil && head == baseHead &&
			(blockingReviewUnchanged || (!satisfiedNoop &&
				!branchHasWorkOverBase(ctx, workdir, req.WorkItem.ParentID))) {
			detail := strings.TrimSpace(result.Response)
			if detail == "" {
				detail = "delegate returned a partial result and produced no commit"
			}
			return StepResult{Status: StepChanges, Detail: safeDiagnostic(detail),
				CostUSD: cost, CostUnknown: costUnknown}, nil
		}
	}
	if !docs {
		if err := r.verifier.Verify(ctx, workdir); err != nil {
			return StepResult{Status: StepChanges, Detail: err.Error(), CostUSD: cost, CostUnknown: costUnknown}, nil
		}
	}
	head, err := gitText(ctx, workdir, "rev-parse", "HEAD")
	if err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: head, CostUSD: cost, CostUnknown: costUnknown}, nil
}

func feedbackHasBlockingFinding(feedback *wfe.ReviewFeedback) bool {
	if feedback == nil {
		return false
	}
	for _, finding := range feedback.Findings {
		switch strings.ToLower(strings.TrimSpace(finding.Severity)) {
		case "suggestion", "nit":
		default:
			return true
		}
	}
	return false
}

func (r *NativeRunner) review(ctx context.Context, req StepRequest) (StepResult, error) {
	reviewed, ok := req.Inputs["src"]
	if !ok {
		return StepResult{}, errors.New("review missing src input")
	}
	persona := paramString(req.Node, "persona", paramString(req.Node, "reviewer", "reviewer"))
	prompt := "Review this complete artifact against the proposal. Return only JSON shaped {\"verdict\":\"approve\" or \"changes\" or \"blocked\",\"findings\":[{\"id\":\"...\",\"severity\":\"blocking\",\"location\":\"...\",\"summary\":\"...\",\"recommendation\":\"...\"}]}.\n\nPROPOSAL:\n" + req.Proposal + "\n\nARTIFACT:\n" + string(reviewed.Content)
	workdir := req.WorkItem.Worktree
	if workdir == "" {
		var err error
		workdir, _, err = r.worktrees.Ensure(ctx, req.WorkItem, req.WorkItem.ParentID == "")
		if err != nil {
			return StepResult{}, err
		}
	}
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "review", Persona: persona, Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: workdir})
	if err != nil {
		return StepResult{}, err
	}
	doc, err := extractJSONObject(result.Response)
	if err != nil {
		return malformedReview(reviewed.Hash, persona, err, result.CostUSD), nil
	}
	var parsed panelResponse
	if err := json.Unmarshal(doc, &parsed); err != nil {
		return malformedReview(reviewed.Hash, persona, err, result.CostUSD), nil
	}
	if parsed.Verdict == "approve" && len(parsed.Findings) == 0 {
		return StepResult{Status: StepAdvanced, ArtifactType: "verdict", Artifact: "approved", ContentHash: reviewed.Hash, CostUSD: result.CostUSD, CostUnknown: result.CostUnknown}, nil
	}
	feedback := wfe.ReviewFeedback{SchemaVersion: 1, ArtifactHash: reviewed.Hash}
	for i, finding := range parsed.Findings {
		feedback.Findings = append(feedback.Findings, wfe.Finding{ID: firstNonempty(finding.ID, fmt.Sprintf("%s-%d", persona, i+1)), Persona: persona, Severity: firstNonempty(finding.Severity, "blocking"), Location: finding.Location, Summary: finding.Summary, Recommendation: finding.Recommendation})
	}
	if len(feedback.Findings) == 0 {
		feedback.Findings = append(feedback.Findings, wfe.Finding{ID: "review-invalid", Persona: persona, Severity: "blocking", Summary: "review did not approve and supplied no finding", Recommendation: "review the artifact and provide an actionable finding"})
	}
	return StepResult{Status: StepChanges, Feedback: &feedback, CostUSD: result.CostUSD, CostUnknown: result.CostUnknown}, nil
}

func (r *NativeRunner) checkMergeable(ctx context.Context, req StepRequest) (StepResult, error) {
	prRef, err := prInputRef(req)
	if err != nil {
		return StepResult{}, err
	}
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	workdir, _, err := r.worktrees.Ensure(ctx, item, item.ParentID == "")
	if err != nil {
		return StepResult{}, err
	}
	if checker, ok := r.forge.(interface {
		Mergeable(context.Context, string, string) (bool, error)
	}); ok {
		mergeable, checkErr := checker.Mergeable(ctx, workdir, prRef)
		if checkErr != nil {
			return StepResult{}, checkErr
		}
		if !mergeable {
			return StepResult{Status: StepChanges, Detail: "pull request is not mergeable"}, nil
		}
		return StepResult{Status: StepAdvanced, ArtifactType: "verdict", Artifact: "mergeable"}, nil
	}
	return StepResult{}, errors.New("configured forge does not support mergeability checks")
}

func (r *NativeRunner) commitChanges(ctx context.Context, workdir, stage string) error {
	return commitChangesWithIdentity(ctx, workdir, stage, func() ([]string, error) {
		return r.resolveGitIdentity(ctx, workdir)
	})
}

func (r *NativeRunner) resolveGitIdentity(ctx context.Context, workdir string) ([]string, error) {
	if ident := gitIdentityArgs(); len(ident) > 0 {
		return ident, nil
	}
	provider, ok := r.forge.(GitIdentityProvider)
	if !ok {
		return nil, ErrGitIdentityMissing
	}
	identity, err := provider.Identity(ctx, workdir)
	if err != nil {
		return nil, err
	}
	return []string{"-c", "user.name=" + identity.Name, "-c", "user.email=" + identity.Email}, nil
}

func commitChanges(ctx context.Context, workdir, stage string) error {
	return commitChangesWithIdentity(ctx, workdir, stage, func() ([]string, error) {
		return gitIdentityArgs(), nil
	})
}

func commitChangesWithIdentity(ctx context.Context, workdir, stage string,
	identity func() ([]string, error)) error {
	if _, err := gitText(ctx, workdir, "add", "-A"); err != nil {
		return err
	}
	if err := validateStagedChanges(ctx, workdir); err != nil {
		return err
	}
	cmd := exec.CommandContext(ctx, "git", "-C", workdir, "diff", "--cached", "--quiet")
	if err := cmd.Run(); err == nil {
		return nil
	} else if exit, ok := err.(*exec.ExitError); !ok || exit.ExitCode() != 1 {
		return err
	}
	ident, err := identity()
	if err != nil {
		return fmt.Errorf("resolve git identity: %w", err)
	}
	if len(ident) == 0 {
		return fmt.Errorf("%w: seal AIMEE_GIT_AUTHOR_NAME and AIMEE_GIT_AUTHOR_EMAIL at install",
			ErrGitIdentityMissing)
	}
	_, err = gitText(ctx, workdir, append(ident, "commit", "-m", "wfe: "+stage)...)
	return err
}

const maxDirectGitBlobBytes int64 = 100 * 1024 * 1024

func isCoreDumpName(name string) bool {
	base := filepath.Base(name)
	if base == "core" {
		return true
	}
	if !strings.HasPrefix(base, "core.") || len(base) == len("core.") {
		return false
	}
	for _, r := range base[len("core."):] {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

// validateStagedChanges keeps process crash artifacts and forge-rejected giant
// blobs out of autonomous commits. Core dumps are disposable products of a
// failed verifier, never proposal output, so remove them. Other giant files are
// preserved in the worktree but fail closed with an actionable diagnostic.
func validateStagedChanges(ctx context.Context, workdir string) error {
	cmd := exec.CommandContext(ctx, "git", "-C", workdir, "diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("list staged paths: %s", strings.TrimSpace(string(out)))
	}
	for _, raw := range strings.Split(string(out), "\x00") {
		if raw == "" {
			continue
		}
		path := filepath.Join(workdir, filepath.FromSlash(raw))
		info, statErr := os.Lstat(path)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		if isCoreDumpName(raw) {
			if removeErr := os.Remove(path); removeErr != nil {
				return fmt.Errorf("remove verifier core dump %s: %w", raw, removeErr)
			}
			if _, addErr := gitText(ctx, workdir, "add", "-A", "--", raw); addErr != nil {
				return addErr
			}
			continue
		}
		if info.Size() > maxDirectGitBlobBytes {
			_, _ = gitText(ctx, workdir, "reset", "-q", "HEAD", "--", raw)
			return fmt.Errorf("refusing to commit %s: %d bytes exceeds GitHub's 100 MiB blob limit", raw, info.Size())
		}
	}
	return nil
}

// integrateFeatureBase merges the parent feature branch (aimee/feat/<parentID>)
// into a slice worktree so a dependent slice picks up siblings that merged after
// its worktree was cut. aimee/feat/<parent> is a LOCAL ref that sibling merge
// steps advance in the shared ref store, so this is a pure-local merge — no fetch,
// which also keeps this off the fleet-wide credential rate limiter. It merges
// (never rebases) to preserve SHAs the downstream merge-into-feature and PR diff
// depend on.
//
// Returns ("", nil) when there is nothing to integrate (base absent, or already an
// ancestor of HEAD) or the merge succeeds. On a conflicting/failed merge it never
// leaves a half-merged tree: it aborts, hard-resets as a last resort to guarantee a
// clean worktree, and returns the park reason "base_integration_conflict" so the
// slice surfaces distinctly instead of masquerading as convergence_no_progress or
// poisoning the reused worktree.
// branchHasWorkOverBase reports whether this slice's branch carries any commit
// beyond the feature base it was cut from -- i.e. whether the slice has already
// produced work, regardless of what the current attempt did.
//
// Answers false when the base cannot be resolved. That preserves the existing
// stricter behaviour rather than letting an unresolved base excuse a genuinely
// empty slice: an unverifiable claim of work is not work.
func branchHasWorkOverBase(ctx context.Context, workdir, parentID string) bool {
	if parentID == "" {
		return false
	}
	// One definition of "the feature tip" for every consumer -- see featureBaseRef.
	base := featureBaseRef(ctx, workdir, parentID)
	if base == "" {
		return false
	}
	count, err := gitText(ctx, workdir, "rev-list", "--count", base+"..HEAD")
	if err != nil {
		return false
	}
	return strings.TrimSpace(count) != "" && strings.TrimSpace(count) != "0"
}

// featureBaseRef resolves the ref carrying the feature branch's real tip, or ""
// when it cannot be resolved.
//
// A slice merges through the FORGE, which advances the remote feature branch.
// Nothing advances the local aimee/feat/<parent> ref, so reading it locally hands
// slice N+1 the state the run started with and every slice that already landed is
// invisible. Measured on wi_f96d4b18: local e161dd34, remote da80f8e7, with the
// merged file absent locally.
//
// #2023 fixed this for the base a slice worktree is CUT from. This is the second
// consumer -- the merge that brings the feature branch INTO an existing slice --
// and it had the same stale read, so the intended
// "branch from the feature tip, merge back on completion" cycle only ever saw the
// original tip. Fetch, then prefer the remote ref, falling back to the local one
// when there is no remote (offline or a fresh repo).
func featureBaseRef(ctx context.Context, workdir, parentID string) string {
	if parentID == "" {
		return ""
	}
	local := "aimee/feat/" + parentID
	remote := "origin/" + local
	_, _ = gitText(ctx, workdir, "fetch", "--quiet", "origin",
		"+refs/heads/"+local+":refs/remotes/"+remote)
	if _, err := gitText(ctx, workdir, "rev-parse", "--verify", "--quiet", remote+"^{commit}"); err == nil {
		return remote
	}
	if _, err := gitText(ctx, workdir, "rev-parse", "--verify", "--quiet", local+"^{commit}"); err == nil {
		return local
	}
	return ""
}

func integrateFeatureBase(ctx context.Context, workdir, parentID string) (string, error) {
	return integrateFeatureBaseWithIdentity(ctx, workdir, parentID, func() ([]string, error) {
		return gitIdentityArgs(), nil
	})
}

func (r *NativeRunner) integrateFeatureBase(ctx context.Context, workdir, parentID string) (string, error) {
	return integrateFeatureBaseWithIdentity(ctx, workdir, parentID, func() ([]string, error) {
		return r.resolveGitIdentity(ctx, workdir)
	})
}

func integrateFeatureBaseWithIdentity(ctx context.Context, workdir, parentID string,
	identity func() ([]string, error)) (string, error) {
	base := featureBaseRef(ctx, workdir, parentID)
	// The feature branch may not exist yet (first generation, before any slice has
	// merged into it) — nothing to integrate.
	if base == "" {
		return "", nil
	}
	// Already contains the base tip: merge would be a no-op.
	if _, err := gitText(ctx, workdir, "merge-base", "--is-ancestor", base, "HEAD"); err == nil {
		return "", nil
	}
	// A fast-forward creates no commit and therefore needs no identity.
	if _, err := gitText(ctx, workdir, "merge", "--ff-only", base); err == nil {
		return "", nil
	}
	ident, err := identity()
	if err != nil {
		return "", fmt.Errorf("resolve git identity: %w", err)
	}
	if len(ident) == 0 {
		return "", ErrGitIdentityMissing
	}
	if _, err := gitText(ctx, workdir, append(ident, "merge", "--no-edit", base)...); err == nil {
		return "", nil
	}
	// Merge failed (conflict or otherwise): restore a clean worktree before parking.
	_, _ = gitText(ctx, workdir, "merge", "--abort")
	if status, _ := gitText(ctx, workdir, "status", "--porcelain"); status != "" {
		_, _ = gitText(ctx, workdir, "reset", "--hard", "HEAD")
	}
	return "base_integration_conflict", nil
}

func (r *NativeRunner) freeze(ctx context.Context, req StepRequest) (StepResult, error) {
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	workdir, _, err := r.worktrees.Ensure(ctx, item, item.ParentID == "")
	if err != nil {
		return StepResult{}, err
	}
	diff, err := frozenWorktreeDiff(ctx, item, workdir)
	if err != nil {
		return StepResult{}, err
	}
	if strings.TrimSpace(diff) == "" {
		// The slice produced no net change vs its base — its work is already present
		// (e.g. a sibling merged it and base-integration pulled it in) or the task was
		// a no-op. Freezing an empty diff sends an empty artifact through review, which
		// rejects it, looping the slice to convergence_no_progress. There is nothing to
		// review, PR, or merge, so complete the slice as an accepted no-op.
		return StepResult{Status: StepAccepted, Detail: "no-op: empty diff vs base"}, nil
	}
	if item.ParentID != "" {
		base, err := frozenWorktreeBase(ctx, item, workdir)
		if err != nil {
			return StepResult{}, err
		}
		creates, err := frozenWorktreeCreates(ctx, workdir, base)
		if err != nil {
			return StepResult{}, err
		}
		conflict, err := r.db.ClaimFrozenCreates(ctx, item.ParentID, item.ID, creates)
		if err != nil {
			return StepResult{}, err
		}
		if conflict != nil {
			return StepResult{Status: StepFailed, Detail: fmt.Sprintf(
				"sibling frozen-diff collision: path %q was divergently created by slices %s and %s",
				conflict.Path, conflict.ExistingWorkItem, conflict.ConflictingWorkItem)}, nil
		}
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "frozen_diff", Artifact: diff, ContentHash: wfe.Hash([]byte(diff))}, nil
}

func frozenWorktreeBase(ctx context.Context, item db1.WorkItem, workdir string) (string, error) {
	base := ""
	if item.ParentID != "" {
		// Slice PRs merge through the forge, which advances the remote feature
		// branch while the local aimee/feat/<parent> ref stays at the run's
		// starting point.  Freeze against the same fetched feature tip used by
		// slice creation/integration; otherwise every later slice's review
		// artifact incorrectly includes all previously merged sibling work.
		base = featureBaseRef(ctx, workdir, item.ParentID)
		if base == "" {
			return "", errors.New("parent feature branch is unavailable")
		}
	} else {
		trunk, e := repoDefaultBranch(ctx, workdir)
		if e != nil {
			return "", e
		}
		base = "origin/" + trunk
	}
	return base, nil
}

func frozenWorktreeDiff(ctx context.Context, item db1.WorkItem, workdir string) (string, error) {
	base, err := frozenWorktreeBase(ctx, item, workdir)
	if err != nil {
		return "", err
	}
	diff, err := gitText(ctx, workdir, "--no-pager", "diff", "--full-index", base+"...HEAD")
	if err != nil {
		return "", err
	}
	return diff, nil
}

func frozenWorktreeCreates(ctx context.Context, workdir, base string) ([]db1.FrozenCreate, error) {
	cmd := exec.CommandContext(ctx, "git", "-C", workdir, "diff", "--name-only", "--diff-filter=A",
		"--no-renames", "-z", base+"...HEAD")
	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("list frozen created paths: %w: %s", err, strings.TrimSpace(string(output)))
	}
	parts := bytes.Split(output, []byte{0})
	creates := make([]db1.FrozenCreate, 0, len(parts))
	for _, raw := range parts {
		if len(raw) == 0 {
			continue
		}
		path := string(raw)
		hash, err := gitText(ctx, workdir, "rev-parse", "HEAD:"+path)
		if err != nil {
			return nil, fmt.Errorf("resolve frozen created path %q: %w", path, err)
		}
		creates = append(creates, db1.FrozenCreate{Path: path, ContentHash: hash})
	}
	return creates, nil
}

type panelFinding struct {
	ID             string `json:"id"`
	Severity       string `json:"severity"`
	Location       string `json:"location"`
	Summary        string `json:"summary"`
	Recommendation string `json:"recommendation"`
}
type panelAlignment struct {
	Status  string `json:"status"`
	Summary string `json:"summary"`
}
type panelResponse struct {
	RunID                    string         `json:"run_id"`
	ArtifactHash             string         `json:"artifact_hash"`
	ArtifactStage            string         `json:"artifact_stage"`
	OriginalRequestAlignment panelAlignment `json:"original_request_alignment"`
	Verdict                  string         `json:"verdict"`
	Findings                 []panelFinding `json:"findings"`
}
type panelSeat struct {
	persona, selector, participant string
	ordinal                        int
}

// ensureRoundtableDeadlineFits avoids spending an expiring workflow window on
// a panel that cannot possibly reach its last configured phase. Analysis and
// discussion share one panel deadline; an enabled chairman receives a second.
// Returning DeadlineExceeded before dispatch lets the scheduler reopen the
// workflow with a fresh wall window instead of cancelling a billed chairman
// and rerunning the whole panel.
func ensureRoundtableDeadlineFits(ctx context.Context, panel roundtablecfg.Panel) error {
	deadline, ok := ctx.Deadline()
	if !ok || panel.DeadlineMS <= 0 {
		return nil
	}
	phases := int64(1)
	if panel.ChairmanEnabled {
		phases++
	}
	maxDuration := time.Duration(1<<63 - 1)
	maxDeadlineMS := int64(maxDuration/time.Millisecond) / phases
	if int64(panel.DeadlineMS) > maxDeadlineMS {
		return fmt.Errorf("roundtable deadline budget overflows duration: deadline_ms=%d phases=%d: %w",
			panel.DeadlineMS, phases, context.DeadlineExceeded)
	}
	required := time.Duration(int64(panel.DeadlineMS)*phases) * time.Millisecond
	reserve := required / 20
	if reserve > delegateDeadlineGraceReserve {
		reserve = delegateDeadlineGraceReserve
	}
	required += reserve
	remaining := time.Until(deadline)
	if remaining <= required {
		return fmt.Errorf("roundtable phases do not fit workflow wall budget: remaining=%s required=%s phases=%d: %w",
			remaining.Round(time.Millisecond), required, phases, context.DeadlineExceeded)
	}
	return nil
}

func (r *NativeRunner) roundtable(ctx context.Context, req StepRequest) (StepResult, error) {
	lenses := panelSeats(req.Node)
	if len(lenses) == 0 {
		// A saved roundtable owns its exact seats and personas. Workflow-local panel
		// metadata is only an optional lens/pin overlay; it must not be required to
		// enter the one shared roundtable route. The direct fallback consumes only
		// the first two lenses by contract.
		for _, persona := range []string{"original-request", "reviewer"} {
			lenses = append(lenses, panelSeat{persona: persona})
		}
	}
	lensNames := make([]string, 0, len(lenses))
	for _, lens := range lenses {
		lensNames = append(lensNames, lens.persona)
	}
	// Reviews are convened by the roundtable module over the bus. Without a
	// reviewer there is no path to one, so park rather than pass through: a
	// review that never happened must be visible as a park, not as a verdict.
	if r.reviews == nil {
		return StepResult{Status: StepPending, PauseReason: "panel_unreachable",
			Detail: "no roundtable reviewer configured; reviews run in the roundtable module over the event bus"}, nil
	}
	reviewed, ok := req.Inputs["src"]
	if !ok {
		return StepResult{}, errors.New("roundtable missing src input")
	}
	// The author looped back without changing the artifact, so a fresh panel
	// would reach the same verdict at full cost. Re-serve the existing findings
	// instead of paying for a re-review of identical bytes.
	if req.Feedback != nil && req.Feedback.ArtifactHash == reviewed.Hash && len(req.Feedback.Findings) > 0 {
		unchanged := *req.Feedback
		return StepResult{Status: StepChanges, Feedback: &unchanged,
			Detail: "artifact unchanged since the previous review; prior findings still apply"}, nil
	}
	workdir := req.WorkItem.Worktree
	if workdir == "" && req.WorkItem.Repo != "" {
		var err error
		workdir, _, err = r.worktrees.Ensure(ctx, req.WorkItem, req.WorkItem.ParentID == "")
		if err != nil {
			return StepResult{}, err
		}
	}
	req.WorkItem.Worktree = workdir

	// The saved panel is named, not resolved here: the module owns its preset
	// store, and resolving a second time in the control plane is how the two
	// sides previously ended up with different notions of the same panel.
	result, err := r.reviews.Review(ctx, roundtablecfg.ReviewRequest{
		Artifact:         string(reviewed.Content),
		OriginalRequest:  req.Proposal,
		ArtifactStage:    reviewed.Type,
		Roundtable:       paramString(req.Node, "roundtable", ""),
		Workdir:          workdir,
		RunID:            req.WorkItem.ID,
		Stage:            req.Node.ID,
		ExecutionVersion: req.WorkItem.UpdatedAt,
		ReplayOnly:       req.ReplayOnly,
		CostLimitUSD:     req.CostLimitUSD,
		Focus:            paramString(req.Node, "focus", ""),
		Lenses:           lensNames,
		Pins:             panelPins(req.Node),
	})
	if err != nil {
		// A lost replay is not a park: retrying reproduces the same absence, and
		// only the engine's reservation recovery can resolve it.
		if errors.Is(err, roundtablecfg.ErrReplayUnavailable) {
			return StepResult{CostUSD: result.CostUSD, CostUnknown: result.CostUnknown},
				fmt.Errorf("%w: %w", ErrDelegateReplayUnavailable, err)
		}
		// A review the panel rejects as invalid fails the step rather than
		// parking it. Parking exists for a runner that might come back; this
		// request will be refused identically every time, so retrying it just
		// resubmits the same rejection every few seconds until someone notices.
		// The panel logs which part it refused; the status is all that crosses
		// the wire.
		var status *bus.ModuleCallStatusError
		if errors.As(err, &status) && status.Status == bus.ModuleStatusInvalidRequest {
			return StepResult{Status: StepFailed, CostUSD: result.CostUSD,
				CostUnknown: result.CostUnknown,
				Detail:      "roundtable rejected the review request as invalid; see the module log for which part"}, nil
		}
		return StepResult{}, err
	}
	return roundtableStepResult(result, reviewed.Hash), nil
}

// roundtableStepResult maps the panel's own verdict onto a workflow step. The
// panel deliberately does not know about steps: it is a module process whose
// result crosses a process boundary, so it reports what the review concluded
// and the engine decides what that means for the run.
func roundtableStepResult(result roundtablecfg.RunResult, artifactHash string) StepResult {
	rt := result
	step := StepResult{CostUSD: result.CostUSD, CostUnknown: result.CostUnknown, Roundtable: &rt}
	switch result.Status {
	case roundtablecfg.StatusApproved:
		step.Status = StepAdvanced
		step.ArtifactType = "verdict"
		step.Artifact = "approved"
		step.ContentHash = artifactHash
		step.Feedback = result.Feedback
	case roundtablecfg.StatusChanges:
		step.Status = StepChanges
		step.Feedback = result.Feedback
	default:
		step.Status = StepPending
		step.PauseReason = result.PauseReason
		step.Detail = result.Detail
		if result.PauseReason == "request_unimplementable" {
			step.Feedback = result.Feedback
		}
	}
	return step
}

func normalizeRoundtableStage(raw string) (string, bool) {
	stage := strings.ToLower(strings.TrimSpace(raw))
	switch stage {
	case "intent", "plan", "frozen_diff":
		return stage, true
	default:
		return "", false
	}
}

func panelFailureCategory(err error, transport bool) string {
	switch {
	case errors.Is(err, context.DeadlineExceeded):
		return "deadline"
	case errors.Is(err, ErrDelegateReplayUnavailable):
		return "replay_unavailable"
	case errors.Is(err, ErrDelegateUnassignedExpired):
		return "unassigned_expired"
	case isCapacityBackpressure(err):
		return "capacity_backpressure"
	case errors.Is(err, ErrDelegateTerminal):
		return "delegate_terminal"
	case transport:
		return "delegate_error"
	default:
		return "malformed_after_repair"
	}
}

func remainingCostLimit(limit, spent float64) float64 {
	if limit <= 0 {
		return 0
	}
	remaining := limit - spent
	if remaining < 0 {
		return 0
	}
	return remaining
}

// panelVerdictError reports why a parsed seat response is not a usable verdict.
// Approve carries no findings and changes carries at least one; anything else is
// a reviewer that contradicted itself, which says nothing about the artifact.
// panelVerdict is the one normalization of a seat or chairman verdict. Both
// paths must read the same value: validating one form and branching on another
// silently turns a usable verdict into a non-vote.
func panelVerdict(parsed panelResponse) string {
	return strings.ToLower(strings.TrimSpace(parsed.Verdict))
}

func (r *NativeRunner) foreach(ctx context.Context, req StepRequest) (StepResult, error) {
	packetsArtifact, ok := req.Inputs["packets"]
	if !ok {
		return StepResult{}, errors.New("foreach.workflow requires packets input")
	}
	var packetPlan struct {
		Packets []json.RawMessage `json:"packets"`
	}
	if err := json.Unmarshal(packetsArtifact.Content, &packetPlan); err != nil || len(packetPlan.Packets) == 0 {
		return StepResult{}, errors.New("foreach packet plan is missing or invalid")
	}
	maxChildren := paramInt(req.Node, "max_children", 16)
	if len(packetPlan.Packets) > maxChildren {
		return StepResult{Status: StepPending, PauseReason: "fanout_limit", Detail: fmt.Sprintf("%d packets exceed configured max_children %d", len(packetPlan.Packets), maxChildren)}, nil
	}
	childName := paramString(req.Node, "workflow", "slice")
	definition, err := r.workflows.Pin(childName)
	if err != nil {
		return StepResult{}, err
	}
	start := definition.Start
	if start == "" {
		start = definition.Nodes[0].ID
	}
	loopCount, err := r.db.StageLoopCount(ctx, req.WorkItem.ID, req.Node.ID)
	if err != nil {
		return StepResult{}, err
	}
	generation := fmt.Sprintf("%s.g%d", packetsArtifact.Hash[:10], loopCount)
	allChildren, err := r.db.Children(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	byID := make(map[string]db1.WorkItem, len(allChildren))
	for _, child := range allChildren {
		byID[child.ID] = child
	}
	created := make([]string, 0, len(packetPlan.Packets))
	children := make([]db1.WorkItem, 0, len(packetPlan.Packets))
	for i, packet := range packetPlan.Packets {
		id := fmt.Sprintf("%s.s%s.%d", req.WorkItem.ID, generation, i)
		if child, exists := byID[id]; exists {
			children = append(children, child)
			continue
		}
		if err := r.artifacts.PutProposal(id, packet); err != nil {
			r.rollbackChildren(ctx, created)
			return StepResult{}, err
		}
		// The child id already carries the fanout generation and is unique, so
		// deriving the identity from it keeps UNIQUE(repo, proposal_path)
		// satisfied when refinement regenerates byte-identical packet content.
		// Keying on the packet hash alone collided with the prior generation's
		// row and wedged the parent in slices. The content hash stays appended
		// for operator diagnosis. Same-generation retries still deduplicate on
		// the id above, before this insert is reached.
		if err := r.db.CreateWorkItem(ctx, db1.CreateWorkItem{ID: id, Repo: req.WorkItem.Repo, ProposalPath: "packet:" + id + ":" + wfe.Hash(packet), WorkflowName: childName, WorkflowVersion: definition.Version, StartStage: start, Mode: "autonomous", ParentID: req.WorkItem.ID}); err != nil {
			r.rollbackChildren(ctx, created)
			return StepResult{}, err
		}
		created = append(created, id)
		child, loadErr := r.db.WorkItem(ctx, id)
		if loadErr != nil {
			r.rollbackChildren(ctx, created)
			return StepResult{}, loadErr
		}
		children = append(children, child)
	}
	if len(created) > 0 {
		return StepResult{Status: StepPending, PauseReason: "slices_running", Detail: fmt.Sprintf("spawned %d child workflows for packet generation %s", len(created), generation)}, nil
	}
	accepted := 0
	for _, child := range children {
		switch child.State {
		case "accepted":
			accepted++
		case "rejected", "stopped", "abandoned":
			return StepResult{Status: StepChanges, Detail: "child " + child.ID + " ended " + child.State}, nil
		}
	}
	if accepted < len(children) {
		return StepResult{Status: StepPending, PauseReason: "slices_running", Detail: fmt.Sprintf("%d/%d child workflows complete", accepted, len(children))}, nil
	}
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	workdir, branch, err := r.worktrees.Ensure(ctx, item, true)
	if err != nil {
		return StepResult{}, err
	}
	// Fetch the feature branch and ff-merge what the fetch retrieved. `git fetch
	// origin <branch>` with an explicit refspec updates ONLY FETCH_HEAD, not the
	// refs/remotes/origin/<branch> tracking ref — so merging "origin/<branch>" fails
	// with "not something we can merge" the first time this branch is fetched here
	// (the tracking ref never existed). Merge FETCH_HEAD, which the fetch just set to
	// the remote tip. This step is only reached once every slice has merged its sub-PR
	// into the feature branch, so FETCH_HEAD is exactly the assembled feature tip.
	if _, err := gitText(ctx, workdir, "fetch", "origin", branch); err != nil {
		return StepResult{}, err
	}
	if _, err := gitText(ctx, workdir, "merge", "--ff-only", "FETCH_HEAD"); err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: wfe.Hash([]byte(branch))}, nil
}

func (r *NativeRunner) rollbackChildren(ctx context.Context, ids []string) {
	for _, id := range ids {
		_ = r.db.Stop(ctx, id)
		_ = r.db.Delete(ctx, id)
	}
}

func (r *NativeRunner) prOpen(ctx context.Context, req StepRequest) (StepResult, error) {
	if _, ok := req.Inputs["src"]; !ok {
		return StepResult{}, errors.New("pr.open missing src input")
	}
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	workdir, head, err := r.worktrees.Ensure(ctx, item, item.ParentID == "")
	if err != nil {
		return StepResult{}, err
	}
	baseKind := paramString(req.Node, "base", "default")
	base := ""
	switch baseKind {
	case "feature":
		if item.ParentID == "" {
			return StepResult{}, errors.New("base:feature requires a child workflow")
		}
		base = "aimee/feat/" + item.ParentID
	case "trunk", "default":
		// The root repository checkout is the proposal's admitted integration
		// lane. It need not match origin/HEAD (testing versus main, or a
		// deliberately pinned batch branch), and the forge resource plane
		// enforces this same checkout-derived base independently.
		base, err = repoIntegrationBranch(ctx, item.Repo)
		if err != nil {
			return StepResult{}, err
		}
	default:
		base = baseKind
	}
	baseConflict, detail, err := r.refreshPullRequestBase(ctx, workdir, base)
	if err != nil {
		return StepResult{}, err
	}
	if baseConflict {
		return StepResult{Status: StepPending, PauseReason: "base_integration_conflict", Detail: detail}, nil
	}
	spec, err := r.pullRequestSpec(ctx, req, item, workdir, head, base)
	if err != nil {
		return StepResult{}, err
	}
	if err := r.ensureRunnable(ctx, item.ID); err != nil {
		return StepResult{}, err
	}
	pr, err := r.forge.Open(ctx, item.Repo, workdir, head, base, spec)
	if err != nil {
		return StepResult{}, err
	}
	if err := r.db.SetPRRef(ctx, item.ID, pr.Ref); err != nil {
		return StepResult{}, err
	}
	encoded, _ := json.Marshal(pr)
	return StepResult{Status: StepAdvanced, ArtifactType: "pr", Artifact: string(encoded), ContentHash: wfe.Hash(encoded)}, nil
}

// refreshPullRequestBase makes the PR contract describe the remote target that
// the reviewer will actually merge into. A long-running workflow may have been
// admitted from a checkout whose origin/<base> was hours behind; generating the
// body from that stale ref both overstates the diff and hides integration
// conflicts. Fetch the exact target ref, integrate it into the managed head,
// and only then compute and publish the handoff.
func refreshPullRequestBase(ctx context.Context, workdir, base string) (bool, string, error) {
	return refreshPullRequestBaseWithIdentity(ctx, workdir, base, func() ([]string, error) {
		return gitIdentityArgs(), nil
	})
}

func (r *NativeRunner) refreshPullRequestBase(ctx context.Context, workdir, base string) (bool, string, error) {
	return refreshPullRequestBaseWithIdentity(ctx, workdir, base, func() ([]string, error) {
		return r.resolveGitIdentity(ctx, workdir)
	})
}

func refreshPullRequestBaseWithIdentity(ctx context.Context, workdir, base string,
	identity func() ([]string, error)) (bool, string, error) {
	if base == "" || strings.HasPrefix(base, "-") {
		return false, "", fmt.Errorf("invalid pull request base %q", base)
	}
	if _, err := gitText(ctx, workdir, "check-ref-format", "--branch", base); err != nil {
		return false, "", fmt.Errorf("invalid pull request base %q", base)
	}
	status, err := gitText(ctx, workdir, "status", "--porcelain")
	if err != nil {
		return false, "", err
	}
	if status != "" {
		return false, "", errors.New("refuse pull request handoff from a dirty worktree")
	}
	baseRef := "refs/remotes/origin/" + base
	refspec := "+refs/heads/" + base + ":" + baseRef
	if _, err := gitText(ctx, workdir, "fetch", "--no-tags", "origin", refspec); err != nil {
		return false, "", fmt.Errorf("refresh pull request base: %w", err)
	}
	// A fast-forward creates no commit and therefore needs no identity.
	if _, err := gitText(ctx, workdir, "merge", "--ff-only", baseRef); err == nil {
		return false, "", nil
	}
	ident, err := identity()
	if err != nil {
		return false, "", fmt.Errorf("resolve git identity: %w", err)
	}
	if len(ident) == 0 {
		return false, "", ErrGitIdentityMissing
	}
	if _, err := gitText(ctx, workdir, append(ident, "merge", "--no-edit", baseRef)...); err != nil {
		lower := strings.ToLower(err.Error())
		if strings.Contains(lower, "conflict") || strings.Contains(lower, "automatic merge failed") {
			_, _ = gitText(ctx, workdir, "merge", "--abort")
			return true, "remote base changed and conflicts with the assembled proposal; resolve the content conflict, then resume", nil
		}
		return false, "", fmt.Errorf("integrate pull request base: %w", err)
	}
	return false, "", nil
}

func (r *NativeRunner) gateCI(ctx context.Context, req StepRequest) (StepResult, error) {
	prRef, err := prInputRef(req)
	if err != nil {
		return StepResult{}, err
	}
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	workdir, _, err := r.worktrees.Ensure(ctx, item, item.ParentID == "")
	if err != nil {
		return StepResult{}, err
	}
	state, err := r.forge.CI(ctx, workdir, prRef)
	if err != nil {
		return StepResult{}, err
	}
	switch state {
	case CIPassed:
		return StepResult{Status: StepAdvanced, ArtifactType: "verdict", Artifact: "ci_passed"}, nil
	case CIFailed:
		return StepResult{Status: StepChanges, Detail: "CI failed"}, nil
	default:
		return StepResult{Status: StepPending, PauseReason: "ci_pending"}, nil
	}
}

func (r *NativeRunner) merge(ctx context.Context, req StepRequest) (StepResult, error) {
	prRef, err := prInputRef(req)
	if err != nil {
		return StepResult{}, err
	}
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	if item.ParentID == "" {
		return StepResult{}, errors.New("autonomous merge is allowed only for a slice into its parent feature branch")
	}
	workdir, _, err := r.worktrees.Ensure(ctx, item, false)
	if err != nil {
		return StepResult{}, err
	}
	base := "aimee/feat/" + item.ParentID
	if err := r.ensureRunnable(ctx, item.ID); err != nil {
		return StepResult{}, err
	}
	if err := r.forge.Merge(ctx, workdir, prRef, base); err != nil {
		if mergeErrIsConflict(err) {
			return StepResult{Status: StepFailed,
				Detail: "merge conflict needs a content decision, no retry can resolve it: " + err.Error()}, nil
		}
		return StepResult{Status: StepPending, PauseReason: "merge_pending", Detail: err.Error()}, nil
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "none", Artifact: "merged"}, nil
}

func (r *NativeRunner) archive(ctx context.Context, req StepRequest) (StepResult, error) {
	item, err := r.db.WorkItem(ctx, req.WorkItem.ID)
	if err != nil {
		return StepResult{}, err
	}
	workdir, branch, err := r.worktrees.Ensure(ctx, item, true)
	if err != nil {
		return StepResult{}, err
	}
	source := item.SourcePath
	if source == "" {
		return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch}, nil
	}
	from := paramString(req.Node, "from", "docs/proposals/pending")
	to := paramString(req.Node, "to", "docs/proposals/done")
	cleanSource := filepath.Clean(source)
	if !strings.HasPrefix(cleanSource, filepath.Clean(from)+string(filepath.Separator)) {
		return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch}, nil
	}
	destination := filepath.Join(to, filepath.Base(cleanSource))
	if _, statErr := os.Stat(filepath.Join(workdir, cleanSource)); errors.Is(statErr, os.ErrNotExist) {
		return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch}, nil
	}
	if err := os.MkdirAll(filepath.Join(workdir, filepath.Dir(destination)), 0o755); err != nil {
		return StepResult{}, err
	}
	if _, err := gitText(ctx, workdir, "mv", "-f", cleanSource, destination); err != nil {
		return StepResult{}, err
	}
	if err := r.commitChanges(ctx, workdir, "archive proposal"); err != nil {
		return StepResult{}, err
	}
	head, err := gitText(ctx, workdir, "rev-parse", "HEAD")
	if err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: head}, nil
}

func panelSeats(node wfe.Node) []panelSeat {
	panel, _ := node.Params["panel"].(map[string]any)
	if panel == nil {
		return nil
	}
	required := stringSlice(panel["required"])
	eligible := stringSlice(panel["eligible"])
	pins := stringMap(panel["pins"])
	var out []panelSeat
	for _, p := range required {
		selector := pins[p]
		out = append(out, panelSeat{persona: p, selector: selector})
	}
	for _, p := range eligible {
		selector := pins[p]
		out = append(out, panelSeat{persona: p, selector: selector})
	}
	return out
}

func panelPins(node wfe.Node) map[string]string {
	panel, _ := node.Params["panel"].(map[string]any)
	if panel == nil {
		return nil
	}
	return stringMap(panel["pins"])
}

func stringMap(value any) map[string]string {
	out := map[string]string{}
	mapping, _ := value.(map[string]any)
	for key, raw := range mapping {
		if text, ok := raw.(string); ok {
			out[key] = text
		}
	}
	return out
}

func malformedReview(hash, persona string, err error, cost float64) StepResult {
	feedback := &wfe.ReviewFeedback{SchemaVersion: 1, ArtifactHash: hash, Findings: []wfe.Finding{{
		ID: "malformed-review", Persona: persona, Severity: "blocking",
		Summary: "reviewer returned an invalid structured response", Recommendation: err.Error(),
	}}}
	return StepResult{Status: StepChanges, Feedback: feedback, Detail: err.Error(), CostUSD: cost}
}

func prInputRef(req StepRequest) (string, error) {
	artifact, ok := req.Inputs["pr"]
	if !ok {
		return "", errors.New("PR-consuming block missing pr input")
	}
	var value struct {
		Ref string `json:"ref"`
	}
	if err := json.Unmarshal(artifact.Content, &value); err != nil || value.Ref == "" {
		return "", errors.New("PR input is invalid")
	}
	return value.Ref, nil
}

func (r *NativeRunner) ensureRunnable(ctx context.Context, id string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	item, err := r.db.WorkItem(ctx, id)
	if err != nil {
		return err
	}
	if item.State != "active" || item.PauseReason != "" {
		return errors.New("workflow is no longer runnable")
	}
	return nil
}
func stringSlice(value any) []string {
	raw, ok := value.([]any)
	if !ok {
		if s, ok := value.([]string); ok {
			return s
		}
		return nil
	}
	out := make([]string, 0, len(raw))
	for _, v := range raw {
		if s, ok := v.(string); ok && s != "" {
			out = append(out, s)
		}
	}
	return out
}
func paramString(node wfe.Node, key, fallback string) string {
	if v, ok := node.Params[key].(string); ok && v != "" {
		return v
	}
	return fallback
}
func paramInt(node wfe.Node, key string, fallback int) int {
	switch v := node.Params[key].(type) {
	case int:
		return v
	case uint64:
		return int(v)
	case float64:
		return int(v)
	}
	return fallback
}
func paramBool(node wfe.Node, key string) bool {
	switch value := node.Params[key].(type) {
	case bool:
		return value
	case string:
		return strings.EqualFold(value, "true")
	default:
		return false
	}
}
func inputText(req StepRequest, name string) string {
	if value, ok := req.Inputs[name]; ok {
		return string(value.Content)
	}
	return ""
}
func firstNonempty(value, fallback string) string {
	if value != "" {
		return value
	}
	return fallback
}

// extractJSONObject returns the exact bytes of the first parseable top-level
// JSON object. Candidate spans are disjoint and the scan index is monotonic, so
// every byte is scanned once and passed to json.Unmarshal at most once.
func extractJSONObject(text string) ([]byte, error) {
	// Delegate providers sometimes append prose, shell snippets, or a second JSON
	// value despite an "only JSON" prompt. Parsing first-'{' through last-'}' turns
	// that harmless suffix into an infinite workflow refinement loop. Balance one
	// candidate at a time while honoring quoted braces and escapes instead. A
	// balanced but malformed outer object is skipped atomically: a valid-looking
	// nested object must never be promoted to the provider's top-level response.
	// Candidates never overlap, so every input byte is scanned once and belongs to
	// at most one json.Unmarshal call. Total work is therefore linear in the input
	// length without imposing a byte or candidate-count truncation limit. An
	// unterminated string or escape consumes the remainder and fails closed; there
	// cannot be a safely identifiable sibling object after malformed string data.
	const (
		objectOpen  = byte('{')
		objectClose = byte('}')
		arrayOpen   = byte('[')
		arrayClose  = byte(']')
	)
	matches := func(open, close byte) bool {
		return (open == objectOpen && close == objectClose) || (open == arrayOpen && close == arrayClose)
	}
	start := -1
	// Retain both backing arrays across candidates/outer values. Candidates are
	// scanned once; no candidate-count or byte limit truncates the response.
	var delimiters []byte
	var outerDelimiters []byte
	inString := false
	escaped := false
	outerInString := false
	outerEscaped := false
	resetCandidate := func() {
		start = -1
		delimiters = delimiters[:0]
		inString = false
		escaped = false
	}
	for i := 0; i < len(text); i++ {
		c := text[i]
		if start < 0 {
			// A complete top-level array is a different JSON value. Track its typed
			// framing so objects nested inside it can never be promoted as the
			// delegate's top-level object response.
			if len(outerDelimiters) > 0 {
				if outerInString {
					if outerEscaped {
						outerEscaped = false
					} else if c == '\\' {
						outerEscaped = true
					} else if c == '"' {
						outerInString = false
					}
					continue
				}
				switch c {
				case '"':
					outerInString = true
				case objectOpen, arrayOpen:
					outerDelimiters = append(outerDelimiters, c)
				case objectClose, arrayClose:
					if !matches(outerDelimiters[len(outerDelimiters)-1], c) {
						return nil, errors.New("delegate returned structurally ambiguous outer JSON delimiters")
					}
					outerDelimiters = outerDelimiters[:len(outerDelimiters)-1]
				}
				continue
			}
			if c == arrayOpen {
				outerDelimiters = append(outerDelimiters[:0], c)
				outerInString = false
				outerEscaped = false
			} else if c == objectOpen {
				start = i
				delimiters = append(delimiters[:0], c)
			}
			continue
		}
		if inString {
			if escaped {
				escaped = false
				continue
			}
			if c == '\\' {
				escaped = true
			} else if c == '"' {
				inString = false
			}
			continue
		}
		switch c {
		case '"':
			inString = true
		case objectOpen, arrayOpen:
			delimiters = append(delimiters, c)
		case objectClose, arrayClose:
			if len(delimiters) == 0 || !matches(delimiters[len(delimiters)-1], c) {
				// Once typed framing is mismatched, a later object cannot be proven to
				// be a disjoint sibling rather than data nested in the malformed value.
				// Fail closed instead of promoting an attacker/provider-controlled
				// approval object from ambiguous framing.
				return nil, errors.New("delegate returned structurally ambiguous JSON delimiters")
			}
			delimiters = delimiters[:len(delimiters)-1]
			if len(delimiters) == 0 {
				doc := []byte(text[start : i+1])
				var value map[string]any
				if json.Unmarshal(doc, &value) == nil {
					return doc, nil
				}
				// i only advances: no byte from this failed candidate is
				// revisited or promoted as the start of a nested candidate.
				resetCandidate()
			}
		}
	}
	if len(outerDelimiters) > 0 || outerInString || outerEscaped {
		return nil, errors.New("delegate returned unterminated outer JSON value")
	}
	return nil, errors.New("delegate returned no valid JSON object")
}
func validateStructured(kind string, doc []byte) error {
	var root map[string]any
	if err := json.Unmarshal(doc, &root); err != nil {
		return err
	}
	if root["schema_version"] != float64(1) {
		return errors.New("schema_version must be 1")
	}
	if kind == "intent" {
		if strings.TrimSpace(fmt.Sprint(root["summary"])) == "" {
			return errors.New("intent summary is required")
		}
		if len(stringSlice(root["acceptance_criteria"])) == 0 {
			return errors.New("intent acceptance criteria are required")
		}
		return nil
	}
	packets, ok := root["packets"].([]any)
	if !ok || len(packets) == 0 {
		return errors.New("packet plan requires at least one packet")
	}
	ids := make([]string, 0, len(packets))
	dependencies := make(map[string][]string, len(packets))
	for _, raw := range packets {
		packet, ok := raw.(map[string]any)
		if !ok {
			return errors.New("packet must be an object")
		}
		id, _ := packet["packet_id"].(string)
		if id == "" {
			return errors.New("packet_id is required")
		}
		ids = append(ids, id)
		if rawDependencies, exists := packet["dependencies"]; exists {
			values, valid := rawDependencies.([]any)
			if !valid {
				return fmt.Errorf("packet %s dependencies must be an array", id)
			}
			seen := make(map[string]bool, len(values))
			for _, rawDependency := range values {
				dependency, valid := rawDependency.(string)
				dependency = strings.TrimSpace(dependency)
				if !valid || dependency == "" {
					return fmt.Errorf("packet %s has an invalid dependency", id)
				}
				if seen[dependency] {
					return fmt.Errorf("packet %s repeats dependency %s", id, dependency)
				}
				seen[dependency] = true
				dependencies[id] = append(dependencies[id], dependency)
			}
		}
		if len(stringSlice(packet["acceptance_criteria"])) == 0 {
			return fmt.Errorf("packet %s needs acceptance criteria", id)
		}
	}
	sort.Strings(ids)
	for i := 1; i < len(ids); i++ {
		if ids[i] == ids[i-1] {
			return fmt.Errorf("duplicate packet_id %s", ids[i])
		}
	}
	known := make(map[string]bool, len(ids))
	for _, id := range ids {
		known[id] = true
	}
	for id, packetDependencies := range dependencies {
		for _, dependency := range packetDependencies {
			if dependency == id {
				return fmt.Errorf("packet %s cannot depend on itself", id)
			}
			if !known[dependency] {
				return fmt.Errorf("packet %s depends on unknown packet %s", id, dependency)
			}
		}
	}
	visiting := make(map[string]bool, len(ids))
	visited := make(map[string]bool, len(ids))
	var visit func(string) error
	visit = func(id string) error {
		if visiting[id] {
			return fmt.Errorf("packet dependency cycle includes %s", id)
		}
		if visited[id] {
			return nil
		}
		visiting[id] = true
		for _, dependency := range dependencies[id] {
			if err := visit(dependency); err != nil {
				return err
			}
		}
		visiting[id] = false
		visited[id] = true
		return nil
	}
	for _, id := range ids {
		if err := visit(id); err != nil {
			return err
		}
	}
	return nil
}
