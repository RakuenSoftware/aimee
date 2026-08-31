package engine

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

type StepStatus string

const (
	StepAdvanced StepStatus = "advanced"
	StepChanges  StepStatus = "changes"
	StepPending  StepStatus = "pending"
	StepFailed   StepStatus = "failed"
	// StepAccepted completes the work item as a no-op success now, skipping any
	// remaining stages. Used when a step determines there is nothing left to do
	// (e.g. freeze finds an empty diff because the slice's work is already present
	// in the base) — proceeding would only freeze/review/PR an empty artifact and
	// loop to convergence_no_progress.
	StepAccepted StepStatus = "accepted"
)

type StepRequest struct {
	WorkItem db1.WorkItem `json:"work_item"`
	Node     wfe.Node     `json:"node"`
	// Proposal is the immutable originating request. The historical field name
	// is retained on the wire, but review prompts expose it as ORIGINAL REQUEST.
	Proposal string                  `json:"proposal"`
	Inputs   map[string]wfe.Artifact `json:"inputs,omitempty"`
	Feedback *wfe.ReviewFeedback     `json:"feedback,omitempty"`
	// RetryDetail is the durable diagnostic from the immediately preceding
	// failure of this stage. It lets a repair delegate address a known verifier
	// failure instead of rediscovering it from scratch.
	RetryDetail string              `json:"retry_detail,omitempty"`
	Block       wfe.BlockDefinition `json:"block_definition"`
	// CostLimitUSD is the durable reservation granted to this invocation. A
	// capped runner must pass it to every billable resource-plane call and must
	// not return a cost above it.
	CostLimitUSD float64 `json:"cost_limit_usd,omitempty"`
	// ReplayOnly means actual provider spend was already reconciled but its
	// lifecycle commit failed. Runners may reuse durable results but must not
	// launch new billable work.
	ReplayOnly bool `json:"replay_only,omitempty"`
}

type StepResult struct {
	Status       StepStatus          `json:"status"`
	ArtifactType string              `json:"artifact_type,omitempty"`
	Artifact     string              `json:"artifact,omitempty"`
	ContentHash  string              `json:"content_hash,omitempty"`
	Feedback     *wfe.ReviewFeedback `json:"feedback,omitempty"`
	PauseReason  string              `json:"pause_reason,omitempty"`
	Detail       string              `json:"detail,omitempty"`
	CostUSD      float64             `json:"cost_usd,omitempty"`
	// CostUnknown means at least one billable call in this step produced no
	// measurement. CostUSD is then a lower bound, so the engine must charge the
	// authorized reservation instead of under-committing measured spend.
	CostUnknown bool                     `json:"cost_unknown,omitempty"`
	Roundtable  *roundtablecfg.RunResult `json:"roundtable,omitempty"`
}

type Runner interface {
	Run(context.Context, StepRequest) (StepResult, error)
}

// maxRunnerFailuresWithoutProgress bounds consecutive runner-failure parks at
// one stage before the item is parked for a human instead of auto-resumed.
const maxRunnerFailuresWithoutProgress = 8

// maxCapacityWaitsWithoutProgress bounds how long a stage waits out provider
// capacity before giving up. Capacity backpressure is self-clearing, so this is
// far higher than the runner-failure bound: it exists only so a permanently
// throttled credential cannot hold an item forever.
const maxCapacityWaitsWithoutProgress = 240

type Engine struct {
	db            *db1.Store
	artifacts     *wfe.ArtifactStore
	workflows     *wfe.Registry
	runner        Runner
	maxNoProgress int
	budgetMu      sync.Mutex
	budgetLocks   map[string]*sync.Mutex
	invocationSeq uint64
}

func New(db *db1.Store, artifacts *wfe.ArtifactStore, workflowDir string, runner Runner) (*Engine, error) {
	if db == nil || artifacts == nil || workflowDir == "" || runner == nil {
		return nil, errors.New("DB1, artifacts, workflow directory, and runner are required")
	}
	registry, err := wfe.NewRegistry(workflowDir)
	if err != nil {
		return nil, err
	}
	return &Engine{db: db, artifacts: artifacts, workflows: registry, runner: runner,
		maxNoProgress: 3, budgetLocks: make(map[string]*sync.Mutex)}, nil
}

type AdvanceResult struct {
	Ran         bool
	Terminal    bool
	Parked      bool
	Stage       string
	NextStage   string
	State       string
	PauseReason string
}

func (e *Engine) Advance(ctx context.Context, workItemID string) (AdvanceResult, error) {
	item, err := e.db.WorkItem(ctx, workItemID)
	if err != nil {
		return AdvanceResult{}, err
	}
	out := AdvanceResult{Stage: item.Stage, State: item.State, PauseReason: item.PauseReason}
	if item.State != "active" || item.PauseReason != "" {
		out.Terminal = item.State != "active"
		out.Parked = item.PauseReason != ""
		return out, nil
	}
	owner := fmt.Sprintf("%s:%d:%d", item.ID, time.Now().UnixNano(), atomic.AddUint64(&e.invocationSeq, 1))
	budget, err := e.db.ReserveWorkflowBudget(ctx, item.ID, owner)
	if err != nil {
		return out, err
	}
	if !budget.Allowed {
		if budget.Busy {
			return out, nil
		}
		if err := e.db.ParkBudgetTree(ctx, budget.RootID, "", 0); err != nil {
			return out, err
		}
		out.Parked, out.PauseReason = true, "budget_cap"
		return out, nil
	}
	retainBudget := false
	defer func() {
		// Every invocation is durably owned, including uncapped and zero-cost work.
		// Once a runner completes, reconciliation plus a lifecycle transition is
		// the only operation allowed to consume that ownership.
		if !retainBudget {
			_ = e.db.ReleaseWorkflowBudget(context.WithoutCancel(ctx), item.ID, owner)
		}
	}()

	def, err := e.workflows.LoadVersion(item.WorkflowName, item.WorkflowVersion)
	if err != nil {
		return out, e.parkOnError(ctx, item, "workflow_definition_invalid", err)
	}
	node, ok := def.Node(item.Stage)
	if !ok {
		return out, e.parkOnError(ctx, item, "workflow_stage_missing",
			fmt.Errorf("stage %q is absent", item.Stage))
	}
	block, err := e.workflows.BlockVersion(item.WorkflowName, item.WorkflowVersion, node.Block)
	if err != nil {
		return out, e.parkOnError(ctx, item, "workflow_block_unavailable", err)
	}

	if _, err := e.artifacts.Proposal(item.ID); err != nil {
		if item.ProposalPath == "" {
			return out, e.parkOnError(ctx, item, "proposal_missing", err)
		}
		if err := e.artifacts.ImportProposal(item.ID, item.ProposalPath); err != nil {
			return out, e.parkOnError(ctx, item, "proposal_import_failed", err)
		}
	}
	proposal, err := e.artifacts.Proposal(item.ID)
	if err != nil {
		return out, e.parkOnError(ctx, item, "proposal_missing", err)
	}
	req := StepRequest{WorkItem: item, Node: node, Proposal: string(proposal), CostLimitUSD: budget.Amount, ReplayOnly: budget.ReplayOnly}
	req.Block = block
	retryDetail, detailErr := e.db.LatestStageRetryDetail(ctx, item.ID, item.Stage)
	if detailErr != nil {
		return out, detailErr
	}
	req.RetryDetail = retryDetail
	if len(node.In) > 0 {
		req.Inputs = make(map[string]wfe.Artifact, len(node.In))
		for inputName, binding := range node.In {
			producer, _, _ := wfe.ParseBinding(binding)
			artifact, artifactErr := e.artifacts.NodeArtifact(item.ID, producer)
			if artifactErr != nil {
				return out, e.parkOnError(ctx, item, "input_artifact_missing",
					fmt.Errorf("%s <- %s: %w", inputName, binding, artifactErr))
			}
			req.Inputs[inputName] = artifact
		}
	}
	if feedback, err := e.artifacts.Feedback(item.ID); err == nil {
		req.Feedback = &feedback
	}

	stopHeartbeat := make(chan struct{})
	heartbeatDone := make(chan struct{})
	go func() {
		defer close(heartbeatDone)
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-stopHeartbeat:
				return
			case <-ticker.C:
				_ = e.db.HeartbeatWorkflowBudget(context.WithoutCancel(ctx), item.ID, owner)
			}
		}
	}()
	step, err := e.runner.Run(ctx, req)
	// Stop the heartbeat and wait for it to exit before touching the reservation.
	// Awaiting the goroutine guarantees no lease-extending write can land after
	// reconciliation or release, so exact-once ownership does not depend on every
	// terminal transition happening to move the row off 'reserved' first.
	close(stopHeartbeat)
	<-heartbeatDone
	// Runner calls are deliberately outside this lock. Atomic durable reservation
	// permits capped siblings to execute concurrently; only the short actual-cost
	// reconciliation and lifecycle commit remain ordered per root.
	if budget.MaxUSD > 0 {
		e.budgetMu.Lock()
		budgetLock := e.budgetLocks[budget.RootID]
		if budgetLock == nil {
			budgetLock = &sync.Mutex{}
			e.budgetLocks[budget.RootID] = budgetLock
		}
		e.budgetMu.Unlock()
		budgetLock.Lock()
		defer budgetLock.Unlock()
	}
	if err != nil {
		// A replay-only invocation whose durable result is gone can never succeed
		// by retrying: recover it (re-dispatch a recoverable interruption, or park a
		// lost reconciled result for a human) instead of looping runner_unavailable.
		if errors.Is(err, ErrDelegateReplayUnavailable) {
			retainBudget = true // RecoverLostReplay owns the reservation transition.
			redispatch, recErr := e.db.RecoverLostReplay(context.WithoutCancel(ctx), item.ID, item.Stage, owner)
			if recErr != nil {
				return out, recErr
			}
			if redispatch {
				// The item is left runnable with no reservation; the next scheduler
				// pass re-reserves and dispatches fresh work.
				return out, nil
			}
			out.Parked, out.PauseReason = true, "replay_unrecoverable"
			return out, nil
		}
		reason := "runner_unavailable"
		capacity := isCapacityBackpressure(err)
		if errors.Is(err, ErrGitIdentityMissing) {
			// This cannot heal on the scheduler's five-second runner backoff. Park
			// until an operator seals the documented install-time identity and
			// explicitly resumes the run.
			reason = "git_identity_missing"
		} else if capacity {
			// The provider refused the dispatch outright; no delegate ran. Park
			// on a distinct transient reason so this waits out the throttle on a
			// longer backoff instead of burning the runner-failure bound.
			reason = "capacity_backpressure"
		}
		// A stage that keeps failing without ever advancing is not a transient
		// outage: stop auto-resuming it and park for a human. Without this an
		// endlessly-failing delegate retries on the transient backoff forever.
		bound, counter := maxRunnerFailuresWithoutProgress, e.db.RunnerFailuresSinceProgress
		if capacity {
			bound, counter = maxCapacityWaitsWithoutProgress, e.db.CapacityWaitsSinceProgress
		}
		if failures, ferr := counter(context.WithoutCancel(ctx), item.ID, item.Stage); ferr == nil &&
			failures >= bound {
			reason = "delegate_failed"
		}
		if errors.Is(err, context.DeadlineExceeded) {
			reason = "wall_cap"
		}
		var execution *DelegateExecutionError
		dispatched, costKnown, actual := false, false, 0.0
		if errors.As(err, &execution) {
			dispatched, costKnown, actual = execution.Dispatched, execution.CostKnown, execution.CostUSD
		}
		// Retain ownership until the store atomically parks the item and either
		// releases the pre-dispatch reservation, records measured cost, or marks
		// ambiguous post-dispatch spend unresolved for durable replay.
		retainBudget = true
		detail := reason + ": " + safeDiagnostic(err.Error())
		if parkErr := e.db.ParkRunnerFailure(context.WithoutCancel(ctx), item.ID, item.Stage, owner, reason, detail, dispatched, costKnown, actual); parkErr != nil {
			return out, parkErr
		}
		out.Parked, out.PauseReason = true, reason
		return out, nil
	}
	// A completed runner may already have incurred provider spend. From here on
	// the durable reservation is consumed only by reconciliation plus a lifecycle
	// transition (or an explicit tree park), never by this invocation's defer.
	retainBudget = true
	// Unmeasured spend is charged at the authorization the invocation was granted.
	// Committing the reported zero would let a missing audit row silently spend
	// the whole tree budget.
	if step.CostUnknown && budget.Amount > step.CostUSD {
		step.CostUSD = budget.Amount
	}
	allowed, budgetErr := e.db.ReconcileWorkflowBudget(context.WithoutCancel(ctx), item.ID, owner, step.CostUSD)
	if budgetErr != nil {
		return out, budgetErr
	}
	if budget.MaxUSD > 0 {
		if step.CostUSD > budget.Amount {
			if err := e.db.ParkBudgetTree(context.WithoutCancel(ctx), budget.RootID, item.ID, step.CostUSD); err != nil {
				return out, err
			}
			out.Parked, out.PauseReason = true, "budget_cap"
			return out, nil
		}
		if !allowed {
			if err := e.db.ParkBudgetTree(context.WithoutCancel(ctx), budget.RootID, item.ID, step.CostUSD); err != nil {
				return out, err
			}
			out.Parked, out.PauseReason = true, "budget_cap"
			return out, nil
		}
	}
	out.Ran = true
	if node.Block == "author.plan" && step.Status == StepAdvanced {
		if step.Artifact == "" {
			return out, e.parkAfterSpend(ctx, item, "plan_missing", errors.New("runner returned no plan"), step.CostUSD)
		}
		if err := e.artifacts.PutPlan(item.ID, []byte(step.Artifact)); err != nil {
			return out, e.parkAfterSpend(ctx, item, "plan_write_failed", err, step.CostUSD)
		}
		step.ContentHash = wfe.Hash([]byte(step.Artifact))
	}
	if step.Status == StepAdvanced {
		artifactType := step.ArtifactType
		if artifactType == "" {
			artifactType = block.Produces
		}
		artifact, artifactErr := e.artifacts.PutNodeArtifact(item.ID, node.ID, artifactType,
			[]byte(step.Artifact))
		if artifactErr != nil {
			return out, e.parkAfterSpend(ctx, item, "artifact_write_failed", artifactErr, step.CostUSD)
		}
		if step.ContentHash == "" {
			step.ContentHash = artifact.Hash
		}
	}

	switch step.Status {
	case StepAdvanced:
		// An approving gate may still have recorded non-blocking deficiencies —
		// technical debt to act on later. Only the changes path used to persist
		// feedback, so approving silently discarded them and the debt was never
		// written down anywhere an operator could find it.
		if step.Feedback != nil && len(step.Feedback.Findings) > 0 {
			if err := e.artifacts.PutFeedback(item.ID, *step.Feedback); err != nil {
				return out, e.parkAfterSpend(ctx, item, "feedback_write_failed", err, step.CostUSD)
			}
		}
		next := node.Next
		// A gate's pass edge is `on_pass`, not `next`. This holds for every gate,
		// not just gate.roundtable: gate.ci advances a green PR to the merge stage
		// via on_pass. Honoring it only for gate.roundtable made gate.ci fall
		// through to next=="" and finish the slice terminal-"accepted" at ci — so
		// the merge node was never reached and the slice PR was left unmerged.
		if node.OnPass != "" {
			next = node.OnPass
		}
		if next == "" {
			if err := e.db.Finish(ctx, item.ID, node.ID, "accepted", step.Detail,
				step.ContentHash, step.CostUSD); err != nil {
				return out, err
			}
			out.Terminal, out.State = true, "accepted"
			return out, nil
		}
		if err := e.db.Move(ctx, item.ID, node.ID, next, "advance", step.Detail,
			step.ContentHash, step.CostUSD); err != nil {
			return out, err
		}
		out.NextStage = next
		return out, nil

	case StepAccepted:
		// Nothing left to do — complete the item as an accepted no-op, skipping any
		// remaining stages (no empty artifact to review, PR, or merge).
		if err := e.db.Finish(ctx, item.ID, node.ID, "accepted", step.Detail,
			step.ContentHash, step.CostUSD); err != nil {
			return out, err
		}
		out.Terminal, out.State = true, "accepted"
		return out, nil

	case StepChanges:
		if node.OnFail == "" {
			return out, e.parkAfterSpend(ctx, item, "invalid_review_result",
				errors.New("changes requires an on_fail edge"), step.CostUSD)
		}
		if node.Block != "gate.roundtable" && node.Block != "review" {
			parked, err := e.db.RecordRetry(ctx, item.ID, node.ID, node.OnFail, step.Detail, maxIterations(node), step.CostUSD)
			if err != nil {
				return out, err
			}
			out.NextStage = node.OnFail
			out.Parked = parked
			if parked {
				out.PauseReason = "retry_limit"
			}
			return out, nil
		}
		if step.Feedback == nil {
			return out, e.parkAfterSpend(ctx, item, "invalid_review_result", errors.New("review changes requires structured feedback"), step.CostUSD)
		}
		reviewed, ok := req.Inputs["src"]
		if !ok {
			return out, e.parkAfterSpend(ctx, item, "review_artifact_missing",
				errors.New("review gate requires an in.src artifact binding"), step.CostUSD)
		}
		step.Feedback.ArtifactHash = reviewed.Hash
		if err := e.artifacts.PutFeedback(item.ID, *step.Feedback); err != nil {
			return out, e.parkAfterSpend(ctx, item, "feedback_write_failed", err, step.CostUSD)
		}
		encoded, err := json.Marshal(step.Feedback)
		if err != nil {
			return out, e.parkAfterSpend(ctx, item, "feedback_encode_failed", err, step.CostUSD)
		}
		transition, err := e.db.RecordRequestedChanges(ctx, item.ID, node.ID, node.OnFail,
			reviewed.Hash, wfe.Hash(encoded),
			convergencePayload(node.ID, unresolvedBlockers(step.Feedback), step.Feedback,
				paramString(node, "blocker_convergence", "observe")),
			maxIterations(node), e.maxNoProgress, step.CostUSD)
		if err != nil {
			return out, err
		}
		out.NextStage = node.OnFail
		out.Parked = transition.Parked
		out.PauseReason = transition.PauseReason
		return out, nil

	case StepPending:
		reason := step.PauseReason
		if reason == "" {
			reason = "runner_pending"
		}
		detail := strings.TrimSpace(step.Detail)
		if detail == "" {
			detail = reason
		} else {
			detail = reason + ": " + safeDiagnostic(detail)
		}
		// The delegate has already run and its spend is reconciled; a cancelled
		// park loses the transition and strands the reservation in 'actual',
		// which the next replay can only park as replay_unrecoverable.
		if err := e.db.ParkWithDetail(context.WithoutCancel(ctx), item.ID, node.ID, reason, detail, step.CostUSD); err != nil {
			return out, err
		}
		out.Parked, out.PauseReason = true, reason
		return out, nil

	case StepFailed:
		if err := e.db.Finish(ctx, item.ID, node.ID, "rejected", step.Detail,
			step.ContentHash, step.CostUSD); err != nil {
			return out, err
		}
		out.Terminal, out.State = true, "rejected"
		return out, nil
	default:
		return out, e.parkAfterSpend(ctx, item, "invalid_runner_result",
			fmt.Errorf("unknown step status %q", step.Status), step.CostUSD)
	}
}

func (e *Engine) parkOnError(ctx context.Context, item db1.WorkItem, reason string, cause error) error {
	return e.parkAfterSpend(ctx, item, reason, cause, 0)
}

func (e *Engine) parkAfterSpend(ctx context.Context, item db1.WorkItem, reason string, cause error, costUSD float64) error {
	detail := reason
	if cause != nil {
		detail += ": " + safeDiagnostic(cause.Error())
	}
	if err := e.db.ParkWithDetail(context.WithoutCancel(ctx), item.ID, item.Stage, reason, detail, costUSD); err != nil {
		return fmt.Errorf("%s: %v; park failed: %w", reason, cause, err)
	}
	return nil
}

// maxIterations is how many times one step may repeat before it parks.
// The parameter is max_rounds. It replaces max_iters, which named the same
// budget less clearly and was the only one the engine ever read — so a node
// declaring just max_rounds silently ran on the default, and a live plan gate
// reached 63 loops that way.
func maxIterations(node wfe.Node) int {
	const defaultMax = 20
	if n, ok := positiveIntParam(node, "max_rounds"); ok {
		return n
	}
	return defaultMax
}

func positiveIntParam(node wfe.Node, name string) (int, bool) {
	value, ok := node.Params[name]
	if !ok {
		return 0, false
	}
	switch n := value.(type) {
	case int:
		if n > 0 {
			return n, true
		}
	case int64:
		if n > 0 && n <= int64(^uint(0)>>1) {
			return int(n), true
		}
	case uint64:
		if n > 0 && n <= uint64(^uint(0)>>1) {
			return int(n), true
		}
	}
	return 0, false
}

// unresolvedBlockers summarises what a gate is still blocking on, for the park
// detail when it exhausts its rounds. A gate that spent its whole budget without
// clearing these is saying something about the plan or the request, and the park
// is where a human looks first -- "convergence_limit" alone sends them digging
// through the feedback artifact to find out what never got fixed.
//
// The chairman's findings come first. Its verdict overrides the seats', and its
// original-request alignment finding is the closest thing the run has to a stated
// reason the work failed, so it is what a human needs to read first. Each entry
// carries its recommendation, because "what was wrong" without "what to do" still
// leaves the reader opening artifacts.
func unresolvedBlockers(feedback *wfe.ReviewFeedback) string {
	if feedback == nil {
		return ""
	}
	blocking := func(finding wfe.Finding) bool {
		switch strings.ToLower(strings.TrimSpace(finding.Severity)) {
		case "foundational", "blocking":
			return strings.TrimSpace(finding.Summary) != ""
		}
		return false
	}
	ordered := make([]wfe.Finding, 0, len(feedback.Findings))
	for _, finding := range feedback.Findings {
		if blocking(finding) && strings.EqualFold(strings.TrimSpace(finding.Persona), "chairman") {
			ordered = append(ordered, finding)
		}
	}
	for _, finding := range feedback.Findings {
		if blocking(finding) && !strings.EqualFold(strings.TrimSpace(finding.Persona), "chairman") {
			ordered = append(ordered, finding)
		}
	}

	clip := func(text string, limit int) string {
		text = strings.Join(strings.Fields(text), " ")
		if len(text) > limit {
			return text[:limit-3] + "..."
		}
		return text
	}
	entries := make([]string, 0, 3)
	for _, finding := range ordered {
		entry := clip(finding.Summary, 200)
		if rec := clip(finding.Recommendation, 120); rec != "" {
			entry += " -> " + rec
		}
		if persona := strings.TrimSpace(finding.Persona); persona != "" {
			entry = "[" + persona + "] " + entry
		}
		entries = append(entries, entry)
		// Three is enough to characterise the blockage; the full review stays in
		// the feedback artifact, and an unbounded detail would bloat every event.
		if len(entries) == 3 {
			break
		}
	}
	return strings.Join(entries, " | ")
}
