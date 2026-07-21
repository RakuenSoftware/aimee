package engine

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type Verifier interface {
	Verify(context.Context, string) error
}

type CommandVerifier struct{ Command []string }

func (v CommandVerifier) Verify(ctx context.Context, workdir string) error {
	command := v.Command
	if len(command) == 0 {
		command = []string{"aimee", "git", "verify", "--json"}
	}
	cmd := exec.CommandContext(ctx, command[0], command[1:]...)
	cmd.Dir = workdir
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("verify failed: %s", strings.TrimSpace(string(output)))
	}
	return nil
}

type NativeRunner struct {
	db        *db1.Store
	worktrees *WorktreeManager
	agents    AgentClient
	verifier  Verifier
	artifacts *wfe.ArtifactStore
	workflows *wfe.Registry
	forge     Forge
}

func (r *NativeRunner) delegate(ctx context.Context, step StepRequest, request DelegateRequest) (DelegateResult, error) {
	request.WorkItemID = step.WorkItem.ID
	request.Stage = step.Node.ID
	request.ExecutionVersion = step.WorkItem.UpdatedAt
	return r.agents.Delegate(ctx, request)
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
			if err := commitChanges(ctx, workdir, req.Node.ID); err != nil {
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
		if err := commitChanges(ctx, workdir, req.Node.ID); err != nil {
			return StepResult{}, err
		}
		head, err := gitText(ctx, workdir, "rev-parse", "HEAD")
		if err != nil {
			return StepResult{}, err
		}
		return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: head, CostUSD: result.CostUSD}, nil
	}
	return StepResult{Status: StepAdvanced, ArtifactType: block.Produces, Artifact: result.Response, CostUSD: result.CostUSD}, nil
}

func (r *NativeRunner) author(ctx context.Context, req StepRequest, kind string) (StepResult, error) {
	proposal, ok := req.Inputs["proposal"]
	if !ok {
		return StepResult{}, errors.New("author.plan missing proposal input")
	}
	// The proposal input is the immutable workflow-entry request; only its schema name is historical.
	prompt := "Author a complete implementation plan for the original request below. Return only the plan; do not truncate it.\n\nORIGINAL REQUEST:\n" + string(proposal.Content)
	if req.Feedback != nil {
		encoded, _ := json.Marshal(req.Feedback)
		prompt += "\n\nPRIOR REVIEW FEEDBACK TO RESOLVE:\n" + string(encoded)
	}
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "draft", Persona: paramString(req.Node, "persona", "architect"), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: req.WorkItem.Repo})
	if err != nil {
		return StepResult{}, err
	}
	if strings.TrimSpace(result.Response) == "" {
		return StepResult{Status: StepChanges, Detail: "planner returned an empty artifact", CostUSD: result.CostUSD}, nil
	}
	return StepResult{Status: StepAdvanced, ArtifactType: kind, Artifact: result.Response, CostUSD: result.CostUSD}, nil
}

func (r *NativeRunner) structured(ctx context.Context, req StepRequest, kind string) (StepResult, error) {
	var prompt string
	if kind == "intent" {
		prompt = "Scope the engineering task below. Return only JSON shaped {\"schema_version\":1,\"status\":\"unconfirmed\",\"summary\":\"...\",\"rationale\":\"...\",\"acceptance_criteria\":[\"...\"]}. Describe the task, never the bookkeeping record.\n\nTASK:\n" + req.Proposal
	} else {
		source := inputText(req, "plan")
		if source == "" {
			return StepResult{}, errors.New("split requires an in.plan artifact binding")
		}
		prompt = "Decompose the complete approved plan below. Return only JSON shaped {\"schema_version\":1,\"packets\":[{\"packet_id\":\"p1\",\"summary\":\"...\",\"target_blocks\":[\"implement\"],\"dependencies\":[],\"acceptance_criteria\":[\"...\"]}]}. Do not omit work or truncate content.\n\nPLAN:\n" + source
		if req.Feedback != nil {
			encoded, _ := json.Marshal(req.Feedback)
			prompt += "\n\nACCEPTANCE FEEDBACK THAT THE NEW PACKETS MUST RESOLVE:\n" + string(encoded)
		}
	}
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "draft", Persona: paramString(req.Node, "persona", "architect"), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: req.WorkItem.Repo})
	if err != nil {
		return StepResult{}, err
	}
	content, err := extractJSONObject(result.Response)
	if err != nil {
		return StepResult{Status: StepChanges, Detail: err.Error(), CostUSD: result.CostUSD}, nil
	}
	if err := validateStructured(kind, content); err != nil {
		return StepResult{Status: StepChanges, Detail: err.Error(), CostUSD: result.CostUSD}, nil
	}
	typeName := "intent"
	if kind == "packets" {
		typeName = "plan"
	}
	return StepResult{Status: StepAdvanced, ArtifactType: typeName, Artifact: string(content), CostUSD: result.CostUSD}, nil
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

func (r *NativeRunner) mutate(ctx context.Context, req StepRequest, docs bool) (StepResult, error) {
	workdir, branch, err := r.worktrees.Ensure(ctx, req.WorkItem, req.WorkItem.ParentID == "")
	if err != nil {
		return StepResult{}, err
	}
	prompt := "Implement the complete approved task in this worktree, run the repository verification, fix failures, and leave the accepted changes in the worktree."
	if docs {
		prompt = "Document the complete implemented change in this worktree. Update the appropriate user and developer documentation and inline comments; leave the accepted changes in the worktree."
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
	var cost float64
	if !docs && paramBool(req.Node, "tdd") {
		testPrompt := "Write the failing tests required by this task before implementation. Run them to confirm they fail for the intended reason, and leave the tests in the worktree.\n\n" + prompt
		testResult, testErr := r.delegate(ctx, req, DelegateRequest{Role: "code", Persona: paramString(req.Node, "test_persona", "qa"), Delegate: paramString(req.Node, "test_delegate", ""), Prompt: testPrompt, Workdir: workdir, Tools: true})
		if testErr != nil {
			return StepResult{}, testErr
		}
		cost += testResult.CostUSD
		if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
			return StepResult{}, err
		}
		if err := commitChanges(ctx, workdir, req.Node.ID+" tests"); err != nil {
			return StepResult{}, err
		}
		prompt += "\n\nTDD: failing tests have already been authored in the worktree. Make them pass without weakening or deleting their assertions."
	}
	result, err := r.delegate(ctx, req, DelegateRequest{Role: "code", Persona: paramString(req.Node, "persona", "engineer"), Delegate: paramString(req.Node, "delegate", ""), Prompt: prompt, Workdir: workdir, Tools: true})
	if err != nil {
		return StepResult{}, err
	}
	cost += result.CostUSD
	if err := r.ensureRunnable(ctx, req.WorkItem.ID); err != nil {
		return StepResult{}, err
	}
	if err := commitChanges(ctx, workdir, req.Node.ID); err != nil {
		return StepResult{}, err
	}
	if !docs {
		if err := r.verifier.Verify(ctx, workdir); err != nil {
			return StepResult{Status: StepChanges, Detail: err.Error(), CostUSD: cost}, nil
		}
	}
	head, err := gitText(ctx, workdir, "rev-parse", "HEAD")
	if err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "branch", Artifact: branch, ContentHash: head, CostUSD: cost}, nil
}

func (r *NativeRunner) review(ctx context.Context, req StepRequest) (StepResult, error) {
	reviewed, ok := req.Inputs["src"]
	if !ok {
		return StepResult{}, errors.New("review missing src input")
	}
	persona := paramString(req.Node, "persona", paramString(req.Node, "reviewer", "reviewer"))
	prompt := "Review this complete artifact against the proposal. Return only JSON shaped {\"verdict\":\"approve\" or \"changes\",\"findings\":[{\"id\":\"...\",\"severity\":\"blocking\",\"location\":\"...\",\"summary\":\"...\",\"recommendation\":\"...\"}]}.\n\nPROPOSAL:\n" + req.Proposal + "\n\nARTIFACT:\n" + string(reviewed.Content)
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
		return StepResult{Status: StepAdvanced, ArtifactType: "verdict", Artifact: "approved", ContentHash: reviewed.Hash, CostUSD: result.CostUSD}, nil
	}
	feedback := wfe.ReviewFeedback{SchemaVersion: 1, ArtifactHash: reviewed.Hash}
	for i, finding := range parsed.Findings {
		feedback.Findings = append(feedback.Findings, wfe.Finding{ID: firstNonempty(finding.ID, fmt.Sprintf("%s-%d", persona, i+1)), Persona: persona, Severity: firstNonempty(finding.Severity, "blocking"), Location: finding.Location, Summary: finding.Summary, Recommendation: finding.Recommendation})
	}
	if len(feedback.Findings) == 0 {
		feedback.Findings = append(feedback.Findings, wfe.Finding{ID: "review-invalid", Persona: persona, Severity: "blocking", Summary: "review did not approve and supplied no finding", Recommendation: "review the artifact and provide an actionable finding"})
	}
	return StepResult{Status: StepChanges, Feedback: &feedback, CostUSD: result.CostUSD}, nil
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

func commitChanges(ctx context.Context, workdir, stage string) error {
	if _, err := gitText(ctx, workdir, "add", "-A"); err != nil {
		return err
	}
	cmd := exec.CommandContext(ctx, "git", "-C", workdir, "diff", "--cached", "--quiet")
	if err := cmd.Run(); err == nil {
		return nil
	} else if exit, ok := err.(*exec.ExitError); !ok || exit.ExitCode() != 1 {
		return err
	}
	_, err := gitText(ctx, workdir, "-c", "user.name=aimee-wfe", "-c", "user.email=wfe@aimee.local", "commit", "-m", "wfe: "+stage)
	return err
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
	base := ""
	if item.ParentID != "" {
		base = "aimee/feat/" + item.ParentID
	} else {
		trunk, e := repoDefaultBranch(ctx, workdir)
		if e != nil {
			return StepResult{}, e
		}
		base = "origin/" + trunk
	}
	diff, err := gitText(ctx, workdir, "--no-pager", "diff", base+"...HEAD")
	if err != nil {
		return StepResult{}, err
	}
	return StepResult{Status: StepAdvanced, ArtifactType: "frozen_diff", Artifact: diff, ContentHash: wfe.Hash([]byte(diff))}, nil
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
	OriginalRequestAlignment panelAlignment `json:"original_request_alignment"`
	Verdict                  string         `json:"verdict"`
	Findings                 []panelFinding `json:"findings"`
}
type panelSeat struct {
	persona, delegate string
	required          bool
}

func (r *NativeRunner) roundtable(ctx context.Context, req StepRequest) (StepResult, error) {
	seats := panelSeats(req.Node)
	if len(seats) == 0 {
		return StepResult{Status: StepPending, PauseReason: "panel_unreachable"}, nil
	}
	reviewed, ok := req.Inputs["src"]
	if !ok {
		return StepResult{}, errors.New("roundtable missing src input")
	}
	workdir := req.WorkItem.Worktree
	if workdir == "" {
		var err error
		workdir, _, err = r.worktrees.Ensure(ctx, req.WorkItem, req.WorkItem.ParentID == "")
		if err != nil {
			return StepResult{}, err
		}
	}
	req.WorkItem.Worktree = workdir
	focus := paramString(req.Node, "focus", "correctness, completeness, security, and test quality")
	basePrompt := "Review the complete artifact against the complete original request. First decide whether the direction actually follows that request: useful refinement is aligned; substituting a different goal or deliverable is drifted; missing context is unclear. Return only JSON shaped {\"original_request_alignment\":{\"status\":\"aligned\" or \"drifted\" or \"unclear\",\"summary\":\"comparison to the original request\"},\"verdict\":\"approve\" or \"changes\",\"findings\":[{\"id\":\"...\",\"severity\":\"blocking\",\"location\":\"...\",\"summary\":\"...\",\"recommendation\":\"...\"}]}. Drifted, unclear, or omitted alignment must use a changes verdict. A changes verdict requires at least one actionable finding. FOCUS: " + focus + ".\n\nORIGINAL REQUEST:\n" + req.Proposal + "\n\nARTIFACT (" + reviewed.Type + "):\n" + string(reviewed.Content)
	maxRounds := paramInt(req.Node, "max_rounds", 1)
	if maxRounds < 1 {
		maxRounds = 1
	}
	var totalCost float64
	var prior string
	for round := 1; round <= maxRounds; round++ {
		prompt := basePrompt
		if prior != "" {
			prompt += "\n\nPRIOR PANEL FINDINGS:\n" + prior + "\n\nReconsider the artifact and the other reviewers' findings. Preserve valid blockers, remove invalid or duplicate blockers, and approve only if no actionable blocker remains."
		}
		feedback, approvals, voters, cost, unreachable := r.runPanelRound(ctx, req, seats, prompt, reviewed.Hash)
		totalCost += cost
		if unreachable != "" {
			return StepResult{Status: StepPending, PauseReason: "panel_unreachable", Detail: unreachable, CostUSD: totalCost}, nil
		}
		quorum := paramInt(req.Node, "quorum", voters)
		if approvals >= quorum && len(feedback.Findings) == 0 {
			return StepResult{Status: StepAdvanced, ArtifactType: "verdict", Artifact: "approved", ContentHash: reviewed.Hash, CostUSD: totalCost}, nil
		}
		if len(feedback.Findings) == 0 {
			feedback.Findings = append(feedback.Findings, wfe.Finding{ID: "quorum", Persona: "panel", Severity: "blocking", Summary: "required approval quorum was not reached", Recommendation: "revise the artifact and rerun the configured panel"})
		}
		if round == maxRounds {
			return StepResult{Status: StepChanges, Feedback: &feedback, CostUSD: totalCost}, nil
		}
		encoded, _ := json.Marshal(feedback)
		prior = string(encoded)
	}
	return StepResult{}, errors.New("roundtable exhausted without a verdict")
}

func (r *NativeRunner) runPanelRound(ctx context.Context, req StepRequest, seats []panelSeat, prompt, artifactHash string) (wfe.ReviewFeedback, int, int, float64, string) {
	type outcome struct {
		seat   panelSeat
		result panelResponse
		cost   float64
		err    error
	}
	ch := make(chan outcome, len(seats))
	for _, seat := range seats {
		seat := seat
		go func() {
			res, err := r.delegate(ctx, req, DelegateRequest{Role: "review", Persona: seat.persona, Delegate: seat.delegate, Prompt: prompt, Workdir: req.WorkItem.Worktree})
			if err != nil {
				ch <- outcome{seat: seat, err: err}
				return
			}
			doc, e := extractJSONObject(res.Response)
			var parsed panelResponse
			if e == nil {
				e = json.Unmarshal(doc, &parsed)
			}
			ch <- outcome{seat: seat, result: parsed, cost: res.CostUSD, err: e}
		}()
	}
	feedback := wfe.ReviewFeedback{SchemaVersion: 1, ArtifactHash: artifactHash}
	approvals, voters := 0, len(seats)
	var cost float64
	var unreachable []string
	for range seats {
		o := <-ch
		cost += o.cost
		if o.err != nil {
			if o.seat.required {
				unreachable = append(unreachable, o.seat.persona+": "+o.err.Error())
			} else {
				voters--
			}
			continue
		}
		alignment := strings.ToLower(strings.TrimSpace(o.result.OriginalRequestAlignment.Status))
		alignmentOK := alignment == "aligned"
		if !alignmentOK {
			if alignment != "drifted" && alignment != "unclear" {
				alignment = "unclear"
			}
			summary := strings.TrimSpace(o.result.OriginalRequestAlignment.Summary)
			if summary == "" {
				summary = "reviewer did not establish that the direction follows the original request"
			}
			feedback.Findings = append(feedback.Findings, wfe.Finding{
				ID:             o.seat.persona + "-original-request-alignment",
				Persona:        o.seat.persona,
				Severity:       "blocking",
				Summary:        "original-request alignment is " + alignment + ": " + summary,
				Recommendation: "revise the direction so it directly serves the original request, then rerun the panel",
			})
		}
		if o.result.Verdict == "approve" && len(o.result.Findings) == 0 {
			// The feedback finding already prevents advancement; also exclude this
			// vote from quorum so the fail-closed invariant is local and explicit.
			if alignmentOK {
				approvals++
			}
			continue
		}
		if o.result.Verdict != "changes" || len(o.result.Findings) == 0 {
			feedback.Findings = append(feedback.Findings, wfe.Finding{ID: o.seat.persona + "-malformed", Persona: o.seat.persona, Severity: "blocking", Summary: "reviewer returned a contradictory or incomplete verdict", Recommendation: "return approve with no findings, or changes with actionable findings"})
			continue
		}
		for i, f := range o.result.Findings {
			feedback.Findings = append(feedback.Findings, wfe.Finding{ID: firstNonempty(f.ID, fmt.Sprintf("%s-%d", o.seat.persona, i+1)), Persona: o.seat.persona, Severity: firstNonempty(f.Severity, "blocking"), Location: f.Location, Summary: f.Summary, Recommendation: f.Recommendation})
		}
	}
	return feedback, approvals, voters, cost, strings.Join(unreachable, "; ")
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
		if err := r.db.CreateWorkItem(ctx, db1.CreateWorkItem{ID: id, Repo: req.WorkItem.Repo, ProposalPath: "packet:" + wfe.Hash(packet), WorkflowName: childName, WorkflowVersion: definition.Version, StartStage: start, Mode: "autonomous", ParentID: req.WorkItem.ID}); err != nil {
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
	_, _ = gitText(ctx, workdir, "fetch", "origin", branch)
	if _, err := gitText(ctx, workdir, "merge", "--ff-only", "origin/"+branch); err != nil {
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
		base, err = repoDefaultBranch(ctx, workdir)
		if err != nil {
			return StepResult{}, err
		}
	default:
		base = baseKind
	}
	title := paramString(req.Node, "title", "aimee: "+req.WorkItem.ID)
	if err := r.ensureRunnable(ctx, item.ID); err != nil {
		return StepResult{}, err
	}
	pr, err := r.forge.Open(ctx, item.Repo, workdir, head, base, title)
	if err != nil {
		return StepResult{}, err
	}
	if err := r.db.SetPRRef(ctx, item.ID, pr.Ref); err != nil {
		return StepResult{}, err
	}
	encoded, _ := json.Marshal(pr)
	return StepResult{Status: StepAdvanced, ArtifactType: "pr", Artifact: string(encoded), ContentHash: wfe.Hash(encoded)}, nil
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
	if err := commitChanges(ctx, workdir, "archive proposal"); err != nil {
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
		out = append(out, panelSeat{p, pins[p], true})
	}
	for _, p := range eligible {
		out = append(out, panelSeat{p, pins[p], false})
	}
	return out
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
	return nil
}
