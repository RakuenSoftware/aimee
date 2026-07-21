package engine

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type StepStatus string

const (
	StepAdvanced StepStatus = "advanced"
	StepChanges  StepStatus = "changes"
	StepPending  StepStatus = "pending"
	StepFailed   StepStatus = "failed"
)

type StepRequest struct {
	WorkItem db1.WorkItem        `json:"work_item"`
	Node     wfe.Node            `json:"node"`
	Proposal string              `json:"proposal"`
	Plan     string              `json:"plan,omitempty"`
	Feedback *wfe.ReviewFeedback `json:"feedback,omitempty"`
}

type StepResult struct {
	Status      StepStatus          `json:"status"`
	Artifact    string              `json:"artifact,omitempty"`
	ContentHash string              `json:"content_hash,omitempty"`
	Feedback    *wfe.ReviewFeedback `json:"feedback,omitempty"`
	PauseReason string              `json:"pause_reason,omitempty"`
	Detail      string              `json:"detail,omitempty"`
	CostUSD     float64             `json:"cost_usd,omitempty"`
}

type Runner interface {
	Run(context.Context, StepRequest) (StepResult, error)
}

type Engine struct {
	db            *db1.Store
	artifacts     *wfe.ArtifactStore
	workflowDir   string
	runner        Runner
	maxNoProgress int
}

func New(db *db1.Store, artifacts *wfe.ArtifactStore, workflowDir string, runner Runner) (*Engine, error) {
	if db == nil || artifacts == nil || workflowDir == "" || runner == nil {
		return nil, errors.New("DB1, artifacts, workflow directory, and runner are required")
	}
	return &Engine{db: db, artifacts: artifacts, workflowDir: workflowDir, runner: runner,
		maxNoProgress: 3}, nil
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

	def, err := wfe.LoadDefinition(filepath.Join(e.workflowDir, item.WorkflowName+".yaml"))
	if err != nil {
		return out, e.parkOnError(ctx, item, "workflow_definition_invalid", err)
	}
	if item.WorkflowVersion != "" && item.WorkflowVersion != def.Version {
		return out, e.parkOnError(ctx, item, "workflow_version_mismatch",
			fmt.Errorf("pinned=%s loaded=%s", item.WorkflowVersion, def.Version))
	}
	node, ok := def.Node(item.Stage)
	if !ok {
		return out, e.parkOnError(ctx, item, "workflow_stage_missing",
			fmt.Errorf("stage %q is absent", item.Stage))
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
	req := StepRequest{WorkItem: item, Node: node, Proposal: string(proposal)}
	if plan, err := e.artifacts.Plan(item.ID); err == nil {
		req.Plan = string(plan)
	} else if !errors.Is(err, os.ErrNotExist) && node.Block == "gate.roundtable" {
		return out, e.parkOnError(ctx, item, "plan_missing", err)
	}
	if feedback, err := e.artifacts.Feedback(item.ID); err == nil {
		req.Feedback = &feedback
	}

	step, err := e.runner.Run(ctx, req)
	if err != nil {
		return out, e.parkOnError(ctx, item, "runner_unavailable", err)
	}
	out.Ran = true
	if node.Block == "author.plan" && step.Status == StepAdvanced {
		if step.Artifact == "" {
			return out, e.parkOnError(ctx, item, "plan_missing", errors.New("runner returned no plan"))
		}
		if err := e.artifacts.PutPlan(item.ID, []byte(step.Artifact)); err != nil {
			return out, e.parkOnError(ctx, item, "plan_write_failed", err)
		}
		step.ContentHash = wfe.Hash([]byte(step.Artifact))
	}

	switch step.Status {
	case StepAdvanced:
		next := node.Next
		if node.Block == "gate.roundtable" && node.OnPass != "" {
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

	case StepChanges:
		if node.Block != "gate.roundtable" || node.OnFail == "" || step.Feedback == nil {
			return out, e.parkOnError(ctx, item, "invalid_review_result",
				errors.New("changes requires a roundtable gate, on_fail edge, and feedback"))
		}
		plan, err := e.artifacts.Plan(item.ID)
		if err != nil {
			return out, e.parkOnError(ctx, item, "plan_missing", err)
		}
		step.Feedback.ArtifactHash = wfe.Hash(plan)
		if err := e.artifacts.PutFeedback(item.ID, *step.Feedback); err != nil {
			return out, e.parkOnError(ctx, item, "feedback_write_failed", err)
		}
		encoded, err := json.Marshal(step.Feedback)
		if err != nil {
			return out, e.parkOnError(ctx, item, "feedback_encode_failed", err)
		}
		transition, err := e.db.RecordRequestedChanges(ctx, item.ID, node.ID, node.OnFail,
			wfe.Hash(plan), wfe.Hash(encoded), maxIterations(node), e.maxNoProgress)
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
		if err := e.db.Park(ctx, item.ID, node.ID, reason, step.CostUSD); err != nil {
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
		return out, e.parkOnError(ctx, item, "invalid_runner_result",
			fmt.Errorf("unknown step status %q", step.Status))
	}
}

func (e *Engine) parkOnError(ctx context.Context, item db1.WorkItem, reason string, cause error) error {
	if err := e.db.Park(ctx, item.ID, item.Stage, reason, 0); err != nil {
		return fmt.Errorf("%s: %v; park failed: %w", reason, cause, err)
	}
	return nil
}

func maxIterations(node wfe.Node) int {
	const defaultMax = 20
	value, ok := node.Params["max_iters"]
	if !ok {
		return defaultMax
	}
	switch n := value.(type) {
	case int:
		if n > 0 {
			return n
		}
	case int64:
		if n > 0 && n <= int64(^uint(0)>>1) {
			return int(n)
		}
	case uint64:
		if n > 0 && n <= uint64(^uint(0)>>1) {
			return int(n)
		}
	}
	return defaultMax
}
