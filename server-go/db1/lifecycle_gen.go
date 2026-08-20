package db1

// GENERATED from src/modules/db1/eventcontract/operations.json by
// scripts/gen_db1_contract.py. Do not edit: add an operation to the catalog
// and regenerate, so the wire and its callers cannot drift apart.
//
// Family 16: lifecycle, event kind 11792.

import (
	"context"
	"fmt"
)

const EventLifecycle uint32 = 11792
const StageLifecycle uint32 = 16

const opWorkItemCreate uint32 = 1
const opWorkItemGet uint32 = 2
const opWorkItemIDByProposal uint32 = 3
const opWorkItemIDByPRRef uint32 = 4
const opWorkItemSetStage uint32 = 5
const opWorkItemSetPRRef uint32 = 6
const opWorkItemSetWorktree uint32 = 7
const opWorkItemSetSubmitter uint32 = 8
const opWorkItemSetParent uint32 = 9
const opWorkItemAbandonChildren uint32 = 10
const opWorkItemChildCounts uint32 = 11
const opWorkItemCountActiveBySubmitter uint32 = 12
const opWorkItemCountRecentBySubmitter uint32 = 13
const opWorkItemSubmitCapped uint32 = 14
const opWorkItemSetTerminal uint32 = 15
const opWorkItemGateApply uint32 = 16
const opWorkItemSetPause uint32 = 17
const opWorkItemClearPause uint32 = 18
const opWorkItemClearPauseIf uint32 = 19
const opWorkItemAddCost uint32 = 20
const opWorkItemSetCostCap uint32 = 21
const opWorkItemIncOverride uint32 = 22
const opWorkItemDelete uint32 = 23
const opWorkItemReapStaleParks uint32 = 24
const opWorkItemList uint32 = 25
const opWorkItemListLru uint32 = 26
const opLifecycleEventAdd uint32 = 27
const opLifecycleEventList uint32 = 28
const opStageAttemptInc uint32 = 29
const opStageAttemptReset uint32 = 30
const opStageAttemptGet uint32 = 31
const opWorkItemRecordOutcome uint32 = 32
const opWfeChildrenList uint32 = 33
const opWfeActiveRootCount uint32 = 34
const opWfeWorkItemIDByGitProposal uint32 = 35
const opWfeExecutedTurnCount uint32 = 36
const opWfeStageLoopCount uint32 = 37
const opWfeRunnerFailuresSinceProgress uint32 = 38
const opWfeCapacityWaitsSinceProgress uint32 = 39
const opWfeDescendantIds uint32 = 40
const opWfeResumeTransient uint32 = 41
const opWfeResumeWallCaps uint32 = 42
const opWfeAbandonExhaustedWallCaps uint32 = 43
const opWfeResumeReadyParents uint32 = 44
const opWfeDelegateJobSave uint32 = 45
const opWfeDelegateJobsTerminalClaim uint32 = 46
const opWfeBudgetReserve uint32 = 47
const opWfeBudgetTotals uint32 = 48
const opWfeBudgetRelease uint32 = 49
const opWfeBudgetHeartbeat uint32 = 50
const opWfeBudgetReconcile uint32 = 51
const opWfeMove uint32 = 52
const opWfeRecordRetry uint32 = 53
const opWfeParkWithDetail uint32 = 54
const opWfeResume uint32 = 55
const opWfeFinish uint32 = 56
const opWfeStopTree uint32 = 57
const opWfeReconcileOrphans uint32 = 58
const opWfeParkBudgetTree uint32 = 59
const opWfeDeleteTree uint32 = 60
const opWfeResolveGate uint32 = 61
const opWfeRejectGate uint32 = 62
const opWfeParkRunnerFailure uint32 = 63
const opWfeRecoverLostReplay uint32 = 64
const opWfeRecordRequestedChanges uint32 = 65
const opWfeClaimFrozenCreates uint32 = 66

// WfeClaimFrozenCreatesItem is one creates entry for wfe_claim_frozen_creates.
type WfeClaimFrozenCreatesItem struct {
	Path        string
	ContentHash string
}

// WorkItemGet is the row work_item_get answers with.
type WorkItemGet struct {
	WorkItemID         string
	Repo               string
	ProposalPath       string
	WorkflowName       string
	WorkflowVersion    string
	CurrentStage       string
	State              string
	Mode               string
	PauseReason        string
	PausedState        string
	ContentHash        string
	PRRef              string
	Worktree           string
	Submitter          string
	ParentID           string
	CumCostUSD         float64
	WorkItemMaxCostUSD float64
	OverrideCount      int
}

// WorkItemList is the row work_item_list answers with.
type WorkItemList struct {
	WorkItemID         string
	Repo               string
	ProposalPath       string
	WorkflowName       string
	WorkflowVersion    string
	CurrentStage       string
	State              string
	Mode               string
	PauseReason        string
	PausedState        string
	ContentHash        string
	PRRef              string
	Worktree           string
	Submitter          string
	ParentID           string
	CumCostUSD         float64
	WorkItemMaxCostUSD float64
	OverrideCount      int
}

// WorkItemListLru is the row work_item_list_lru answers with.
type WorkItemListLru struct {
	WorkItemID         string
	Repo               string
	ProposalPath       string
	WorkflowName       string
	WorkflowVersion    string
	CurrentStage       string
	State              string
	Mode               string
	PauseReason        string
	PausedState        string
	ContentHash        string
	PRRef              string
	Worktree           string
	Submitter          string
	ParentID           string
	CumCostUSD         float64
	WorkItemMaxCostUSD float64
	OverrideCount      int
}

// LifecycleEventList is the row lifecycle_event_list answers with.
type LifecycleEventList struct {
	ID          int64
	Stage       string
	Kind        string
	Actor       string
	Detail      string
	ContentHash string
	CostUSD     float64
	CreatedAt   string
}

// WfeDelegateJobsTerminalClaim is the row wfe_delegate_jobs_terminal_claim answers with.
type WfeDelegateJobsTerminalClaim struct {
	ExecutionKey string
	JobID        int64
}

// WfeBudgetReserve is the row wfe_budget_reserve answers with.
type WfeBudgetReserve struct {
	RootID     string
	MaxUSD     float64
	Amount     float64
	Allowed    int
	Busy       int
	ReplayOnly int
}

// WfeBudgetTotals is the row wfe_budget_totals answers with.
type WfeBudgetTotals struct {
	RootID string
	Spent  float64
	MaxUSD float64
}

// WfeRecordRequestedChanges is the row wfe_record_requested_changes answers with.
type WfeRecordRequestedChanges struct {
	Attempts         int
	IdenticalRepeats int
	Parked           int
	PauseReason      string
}

// WfeClaimFrozenCreates is the row wfe_claim_frozen_creates answers with.
type WfeClaimFrozenCreates struct {
	Path                string
	ExistingWorkItem    string
	ConflictingWorkItem string
}

func (c *Client) WorkItemCreate(ctx context.Context, workItemID string, repo string, proposalPath string, workflowName string, workflowVersion string, startStage string, mode string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, repo, proposalPath, workflowName, workflowVersion, startStage, mode}
	status, _, err := c.callFields(ctx, opWorkItemCreate, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_create", Status: status}
	}
	return nil
}

func (c *Client) WorkItemGet(ctx context.Context, workItemID string) (WorkItemGet, error) {
	var out WorkItemGet
	if c == nil || c.caller == nil {
		return out, ErrConfig
	}
	fields := []string{workItemID}
	status, reply, err := c.callFields(ctx, opWorkItemGet, fields)
	if err != nil {
		return out, err
	}
	if status != statusOK {
		return out, &StatusError{Op: "work_item_get", Status: status}
	}
	if len(reply) != 18 {
		return out, fmt.Errorf("%w: work_item_get wants 18 fields, got %d", ErrMalformed, len(reply))
	}
	out.WorkItemID = reply[0]
	out.Repo = reply[1]
	out.ProposalPath = reply[2]
	out.WorkflowName = reply[3]
	out.WorkflowVersion = reply[4]
	out.CurrentStage = reply[5]
	out.State = reply[6]
	out.Mode = reply[7]
	out.PauseReason = reply[8]
	out.PausedState = reply[9]
	out.ContentHash = reply[10]
	out.PRRef = reply[11]
	out.Worktree = reply[12]
	out.Submitter = reply[13]
	out.ParentID = reply[14]
	if value, parseErr := Atof(reply[15]); parseErr == nil {
		out.CumCostUSD = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atof(reply[16]); parseErr == nil {
		out.WorkItemMaxCostUSD = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atoi(reply[17]); parseErr == nil {
		out.OverrideCount = value
	} else {
		return out, parseErr
	}
	return out, nil
}

func (c *Client) WorkItemIDByProposal(ctx context.Context, repo string, proposalPath string) (string, error) {
	if c == nil || c.caller == nil {
		return "", ErrConfig
	}
	fields := []string{repo, proposalPath}
	status, reply, err := c.callFields(ctx, opWorkItemIDByProposal, fields)
	if err != nil {
		return "", err
	}
	if status != statusOK {
		return "", &StatusError{Op: "work_item_id_by_proposal", Status: status}
	}
	if len(reply) < 1 {
		return "", ErrMalformed
	}
	return reply[0], nil
}

func (c *Client) WorkItemIDByPRRef(ctx context.Context, pRRef string) (string, error) {
	if c == nil || c.caller == nil {
		return "", ErrConfig
	}
	fields := []string{pRRef}
	status, reply, err := c.callFields(ctx, opWorkItemIDByPRRef, fields)
	if err != nil {
		return "", err
	}
	if status != statusOK {
		return "", &StatusError{Op: "work_item_id_by_pr_ref", Status: status}
	}
	if len(reply) < 1 {
		return "", ErrMalformed
	}
	return reply[0], nil
}

func (c *Client) WorkItemSetStage(ctx context.Context, workItemID string, stage string, contentHash string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage, contentHash}
	status, _, err := c.callFields(ctx, opWorkItemSetStage, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_stage", Status: status}
	}
	return nil
}

func (c *Client) WorkItemSetPRRef(ctx context.Context, workItemID string, pRRef string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, pRRef}
	status, _, err := c.callFields(ctx, opWorkItemSetPRRef, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_pr_ref", Status: status}
	}
	return nil
}

func (c *Client) WorkItemSetWorktree(ctx context.Context, workItemID string, worktree string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, worktree}
	status, _, err := c.callFields(ctx, opWorkItemSetWorktree, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_worktree", Status: status}
	}
	return nil
}

func (c *Client) WorkItemSetSubmitter(ctx context.Context, workItemID string, submitter string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, submitter}
	status, _, err := c.callFields(ctx, opWorkItemSetSubmitter, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_submitter", Status: status}
	}
	return nil
}

func (c *Client) WorkItemSetParent(ctx context.Context, workItemID string, parentID string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, parentID}
	status, _, err := c.callFields(ctx, opWorkItemSetParent, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_parent", Status: status}
	}
	return nil
}

func (c *Client) WorkItemAbandonChildren(ctx context.Context, parentID string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{parentID}
	status, reply, err := c.callFields(ctx, opWorkItemAbandonChildren, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_abandon_children", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemChildCounts(ctx context.Context, parentID string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{parentID}
	status, reply, err := c.callFields(ctx, opWorkItemChildCounts, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_child_counts", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemCountActiveBySubmitter(ctx context.Context, submitter string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{submitter}
	status, reply, err := c.callFields(ctx, opWorkItemCountActiveBySubmitter, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_count_active_by_submitter", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemCountRecentBySubmitter(ctx context.Context, submitter string, secs int) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{submitter, Itoa(secs)}
	status, reply, err := c.callFields(ctx, opWorkItemCountRecentBySubmitter, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_count_recent_by_submitter", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemSubmitCapped(ctx context.Context, workItemID string, repo string, proposalPath string, workflowName string, workflowVersion string, startStage string, submitter string, maxActive int, rateMax int, rateSecs int) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, repo, proposalPath, workflowName, workflowVersion, startStage, submitter, Itoa(maxActive), Itoa(rateMax), Itoa(rateSecs)}
	status, _, err := c.callFields(ctx, opWorkItemSubmitCapped, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_submit_capped", Status: status}
	}
	return nil
}

func (c *Client) WorkItemSetTerminal(ctx context.Context, workItemID string, state string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, state}
	status, _, err := c.callFields(ctx, opWorkItemSetTerminal, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_terminal", Status: status}
	}
	return nil
}

func (c *Client) WorkItemGateApply(ctx context.Context, workItemID string, expectStage string, expectHash string, newStage string, terminalState string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, expectStage, expectHash, newStage, terminalState}
	status, reply, err := c.callFields(ctx, opWorkItemGateApply, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_gate_apply", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemSetPause(ctx context.Context, workItemID string, reason string, pausedState string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, reason, pausedState}
	status, _, err := c.callFields(ctx, opWorkItemSetPause, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_pause", Status: status}
	}
	return nil
}

func (c *Client) WorkItemClearPause(ctx context.Context, workItemID string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID}
	status, _, err := c.callFields(ctx, opWorkItemClearPause, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_clear_pause", Status: status}
	}
	return nil
}

func (c *Client) WorkItemClearPauseIf(ctx context.Context, workItemID string, expectReason string, expectStage string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, expectReason, expectStage}
	status, reply, err := c.callFields(ctx, opWorkItemClearPauseIf, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_clear_pause_if", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemAddCost(ctx context.Context, workItemID string, cost float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, Ftoa(cost)}
	status, _, err := c.callFields(ctx, opWorkItemAddCost, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_add_cost", Status: status}
	}
	return nil
}

func (c *Client) WorkItemSetCostCap(ctx context.Context, workItemID string, cap float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, Ftoa(cap)}
	status, _, err := c.callFields(ctx, opWorkItemSetCostCap, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_set_cost_cap", Status: status}
	}
	return nil
}

func (c *Client) WorkItemIncOverride(ctx context.Context, workItemID string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID}
	status, _, err := c.callFields(ctx, opWorkItemIncOverride, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_inc_override", Status: status}
	}
	return nil
}

func (c *Client) WorkItemDelete(ctx context.Context, workItemID string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID}
	status, _, err := c.callFields(ctx, opWorkItemDelete, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_delete", Status: status}
	}
	return nil
}

func (c *Client) WorkItemReapStaleParks(ctx context.Context, graceSecs int64) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{I64toa(graceSecs)}
	status, reply, err := c.callFields(ctx, opWorkItemReapStaleParks, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "work_item_reap_stale_parks", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemList(ctx context.Context, max int) ([]WorkItemList, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{Itoa(max)}
	status, reply, err := c.callFields(ctx, opWorkItemList, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "work_item_list", Status: status}
	}
	rows, err := Rows(reply, 18)
	if err != nil {
		return nil, err
	}
	out := make([]WorkItemList, 0, len(rows))
	for _, row := range rows {
		var item WorkItemList
		item.WorkItemID = row[0]
		item.Repo = row[1]
		item.ProposalPath = row[2]
		item.WorkflowName = row[3]
		item.WorkflowVersion = row[4]
		item.CurrentStage = row[5]
		item.State = row[6]
		item.Mode = row[7]
		item.PauseReason = row[8]
		item.PausedState = row[9]
		item.ContentHash = row[10]
		item.PRRef = row[11]
		item.Worktree = row[12]
		item.Submitter = row[13]
		item.ParentID = row[14]
		if value, parseErr := Atof(row[15]); parseErr == nil {
			item.CumCostUSD = value
		} else {
			return nil, parseErr
		}
		if value, parseErr := Atof(row[16]); parseErr == nil {
			item.WorkItemMaxCostUSD = value
		} else {
			return nil, parseErr
		}
		if value, parseErr := Atoi(row[17]); parseErr == nil {
			item.OverrideCount = value
		} else {
			return nil, parseErr
		}
		out = append(out, item)
	}
	return out, nil
}

func (c *Client) WorkItemListLru(ctx context.Context, max int) ([]WorkItemListLru, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{Itoa(max)}
	status, reply, err := c.callFields(ctx, opWorkItemListLru, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "work_item_list_lru", Status: status}
	}
	rows, err := Rows(reply, 18)
	if err != nil {
		return nil, err
	}
	out := make([]WorkItemListLru, 0, len(rows))
	for _, row := range rows {
		var item WorkItemListLru
		item.WorkItemID = row[0]
		item.Repo = row[1]
		item.ProposalPath = row[2]
		item.WorkflowName = row[3]
		item.WorkflowVersion = row[4]
		item.CurrentStage = row[5]
		item.State = row[6]
		item.Mode = row[7]
		item.PauseReason = row[8]
		item.PausedState = row[9]
		item.ContentHash = row[10]
		item.PRRef = row[11]
		item.Worktree = row[12]
		item.Submitter = row[13]
		item.ParentID = row[14]
		if value, parseErr := Atof(row[15]); parseErr == nil {
			item.CumCostUSD = value
		} else {
			return nil, parseErr
		}
		if value, parseErr := Atof(row[16]); parseErr == nil {
			item.WorkItemMaxCostUSD = value
		} else {
			return nil, parseErr
		}
		if value, parseErr := Atoi(row[17]); parseErr == nil {
			item.OverrideCount = value
		} else {
			return nil, parseErr
		}
		out = append(out, item)
	}
	return out, nil
}

func (c *Client) LifecycleEventAdd(ctx context.Context, workItemID string, stage string, kind string, actor string, detail string, contentHash string, cost float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage, kind, actor, detail, contentHash, Ftoa(cost)}
	status, _, err := c.callFields(ctx, opLifecycleEventAdd, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "lifecycle_event_add", Status: status}
	}
	return nil
}

func (c *Client) LifecycleEventList(ctx context.Context, workItemID string, max int) ([]LifecycleEventList, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{workItemID, Itoa(max)}
	status, reply, err := c.callFields(ctx, opLifecycleEventList, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "lifecycle_event_list", Status: status}
	}
	rows, err := Rows(reply, 8)
	if err != nil {
		return nil, err
	}
	out := make([]LifecycleEventList, 0, len(rows))
	for _, row := range rows {
		var item LifecycleEventList
		if value, parseErr := Atoi64(row[0]); parseErr == nil {
			item.ID = value
		} else {
			return nil, parseErr
		}
		item.Stage = row[1]
		item.Kind = row[2]
		item.Actor = row[3]
		item.Detail = row[4]
		item.ContentHash = row[5]
		if value, parseErr := Atof(row[6]); parseErr == nil {
			item.CostUSD = value
		} else {
			return nil, parseErr
		}
		item.CreatedAt = row[7]
		out = append(out, item)
	}
	return out, nil
}

func (c *Client) StageAttemptInc(ctx context.Context, workItemID string, stage string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage}
	status, reply, err := c.callFields(ctx, opStageAttemptInc, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "stage_attempt_inc", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) StageAttemptReset(ctx context.Context, workItemID string, stage string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage}
	status, _, err := c.callFields(ctx, opStageAttemptReset, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "stage_attempt_reset", Status: status}
	}
	return nil
}

func (c *Client) StageAttemptGet(ctx context.Context, workItemID string, stage string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage}
	status, reply, err := c.callFields(ctx, opStageAttemptGet, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "stage_attempt_get", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WorkItemRecordOutcome(ctx context.Context, workItemID string, nodeID string, disposition int, state string, pauseReason string, pauseStage string, nextStage string, pRRef string, abandonChildren int, costUSD float64, eventKind string, eventDetail string, eventHash string, parkReason string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, nodeID, Itoa(disposition), state, pauseReason, pauseStage, nextStage, pRRef, Itoa(abandonChildren), Ftoa(costUSD), eventKind, eventDetail, eventHash, parkReason}
	status, _, err := c.callFields(ctx, opWorkItemRecordOutcome, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "work_item_record_outcome", Status: status}
	}
	return nil
}

func (c *Client) WfeChildrenList(ctx context.Context, parentID string, max int) ([]string, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{parentID, Itoa(max)}
	status, reply, err := c.callFields(ctx, opWfeChildrenList, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "wfe_children_list", Status: status}
	}
	return reply, nil
}

func (c *Client) WfeActiveRootCount(ctx context.Context) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{}
	status, reply, err := c.callFields(ctx, opWfeActiveRootCount, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_active_root_count", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeWorkItemIDByGitProposal(ctx context.Context, repo string, proposalPath string, suffix string) (string, error) {
	if c == nil || c.caller == nil {
		return "", ErrConfig
	}
	fields := []string{repo, proposalPath, suffix}
	status, reply, err := c.callFields(ctx, opWfeWorkItemIDByGitProposal, fields)
	if err != nil {
		return "", err
	}
	if status != statusOK {
		return "", &StatusError{Op: "wfe_work_item_id_by_git_proposal", Status: status}
	}
	if len(reply) < 1 {
		return "", ErrMalformed
	}
	return reply[0], nil
}

func (c *Client) WfeExecutedTurnCount(ctx context.Context, workItemID string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID}
	status, reply, err := c.callFields(ctx, opWfeExecutedTurnCount, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_executed_turn_count", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeStageLoopCount(ctx context.Context, workItemID string, stage string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage}
	status, reply, err := c.callFields(ctx, opWfeStageLoopCount, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_stage_loop_count", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeRunnerFailuresSinceProgress(ctx context.Context, workItemID string, stage string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage}
	status, reply, err := c.callFields(ctx, opWfeRunnerFailuresSinceProgress, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_runner_failures_since_progress", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeCapacityWaitsSinceProgress(ctx context.Context, workItemID string, stage string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage}
	status, reply, err := c.callFields(ctx, opWfeCapacityWaitsSinceProgress, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_capacity_waits_since_progress", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeDescendantIds(ctx context.Context, workItemID string, max int) ([]string, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{workItemID, Itoa(max)}
	status, reply, err := c.callFields(ctx, opWfeDescendantIds, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "wfe_descendant_ids", Status: status}
	}
	return reply, nil
}

func (c *Client) WfeResumeTransient(ctx context.Context, pauseReason string, olderThanSecs int) (int64, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{pauseReason, Itoa(olderThanSecs)}
	status, reply, err := c.callFields(ctx, opWfeResumeTransient, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_resume_transient", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi64(reply[0])
}

func (c *Client) WfeResumeWallCaps(ctx context.Context, maxResumes int) (int64, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{Itoa(maxResumes)}
	status, reply, err := c.callFields(ctx, opWfeResumeWallCaps, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_resume_wall_caps", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi64(reply[0])
}

func (c *Client) WfeAbandonExhaustedWallCaps(ctx context.Context, maxResumes int, graceSecs int) (int64, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{Itoa(maxResumes), Itoa(graceSecs)}
	status, reply, err := c.callFields(ctx, opWfeAbandonExhaustedWallCaps, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_abandon_exhausted_wall_caps", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi64(reply[0])
}

func (c *Client) WfeResumeReadyParents(ctx context.Context) (int64, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{}
	status, reply, err := c.callFields(ctx, opWfeResumeReadyParents, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_resume_ready_parents", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi64(reply[0])
}

func (c *Client) WfeDelegateJobSave(ctx context.Context, executionKey string, workItemID string, jobID int64, participantToken string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{executionKey, workItemID, I64toa(jobID), participantToken}
	status, _, err := c.callFields(ctx, opWfeDelegateJobSave, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_delegate_job_save", Status: status}
	}
	return nil
}

func (c *Client) WfeDelegateJobsTerminalClaim(ctx context.Context, max int) ([]WfeDelegateJobsTerminalClaim, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{Itoa(max)}
	status, reply, err := c.callFields(ctx, opWfeDelegateJobsTerminalClaim, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "wfe_delegate_jobs_terminal_claim", Status: status}
	}
	rows, err := Rows(reply, 2)
	if err != nil {
		return nil, err
	}
	out := make([]WfeDelegateJobsTerminalClaim, 0, len(rows))
	for _, row := range rows {
		var item WfeDelegateJobsTerminalClaim
		item.ExecutionKey = row[0]
		if value, parseErr := Atoi64(row[1]); parseErr == nil {
			item.JobID = value
		} else {
			return nil, parseErr
		}
		out = append(out, item)
	}
	return out, nil
}

func (c *Client) WfeBudgetReserve(ctx context.Context, workItemID string, owner string) (WfeBudgetReserve, error) {
	var out WfeBudgetReserve
	if c == nil || c.caller == nil {
		return out, ErrConfig
	}
	fields := []string{workItemID, owner}
	status, reply, err := c.callFields(ctx, opWfeBudgetReserve, fields)
	if err != nil {
		return out, err
	}
	if status != statusOK {
		return out, &StatusError{Op: "wfe_budget_reserve", Status: status}
	}
	if len(reply) != 6 {
		return out, fmt.Errorf("%w: wfe_budget_reserve wants 6 fields, got %d", ErrMalformed, len(reply))
	}
	out.RootID = reply[0]
	if value, parseErr := Atof(reply[1]); parseErr == nil {
		out.MaxUSD = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atof(reply[2]); parseErr == nil {
		out.Amount = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atoi(reply[3]); parseErr == nil {
		out.Allowed = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atoi(reply[4]); parseErr == nil {
		out.Busy = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atoi(reply[5]); parseErr == nil {
		out.ReplayOnly = value
	} else {
		return out, parseErr
	}
	return out, nil
}

func (c *Client) WfeBudgetTotals(ctx context.Context, workItemID string) (WfeBudgetTotals, error) {
	var out WfeBudgetTotals
	if c == nil || c.caller == nil {
		return out, ErrConfig
	}
	fields := []string{workItemID}
	status, reply, err := c.callFields(ctx, opWfeBudgetTotals, fields)
	if err != nil {
		return out, err
	}
	if status != statusOK {
		return out, &StatusError{Op: "wfe_budget_totals", Status: status}
	}
	if len(reply) != 3 {
		return out, fmt.Errorf("%w: wfe_budget_totals wants 3 fields, got %d", ErrMalformed, len(reply))
	}
	out.RootID = reply[0]
	if value, parseErr := Atof(reply[1]); parseErr == nil {
		out.Spent = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atof(reply[2]); parseErr == nil {
		out.MaxUSD = value
	} else {
		return out, parseErr
	}
	return out, nil
}

func (c *Client) WfeBudgetRelease(ctx context.Context, workItemID string, owner string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, owner}
	status, _, err := c.callFields(ctx, opWfeBudgetRelease, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_budget_release", Status: status}
	}
	return nil
}

func (c *Client) WfeBudgetHeartbeat(ctx context.Context, workItemID string, owner string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, owner}
	status, _, err := c.callFields(ctx, opWfeBudgetHeartbeat, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_budget_heartbeat", Status: status}
	}
	return nil
}

func (c *Client) WfeBudgetReconcile(ctx context.Context, workItemID string, owner string, actual float64) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, owner, Ftoa(actual)}
	status, reply, err := c.callFields(ctx, opWfeBudgetReconcile, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_budget_reconcile", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeMove(ctx context.Context, workItemID string, fromStage string, toStage string, kind string, detail string, contentHash string, cost float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, fromStage, toStage, kind, detail, contentHash, Ftoa(cost)}
	status, _, err := c.callFields(ctx, opWfeMove, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_move", Status: status}
	}
	return nil
}

func (c *Client) WfeRecordRetry(ctx context.Context, workItemID string, stage string, toStage string, detail string, maxAttempts int, cost float64) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage, toStage, detail, Itoa(maxAttempts), Ftoa(cost)}
	status, reply, err := c.callFields(ctx, opWfeRecordRetry, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_record_retry", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeParkWithDetail(ctx context.Context, workItemID string, stage string, reason string, detail string, cost float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage, reason, detail, Ftoa(cost)}
	status, _, err := c.callFields(ctx, opWfeParkWithDetail, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_park_with_detail", Status: status}
	}
	return nil
}

func (c *Client) WfeResume(ctx context.Context, workItemID string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID}
	status, _, err := c.callFields(ctx, opWfeResume, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_resume", Status: status}
	}
	return nil
}

func (c *Client) WfeFinish(ctx context.Context, workItemID string, stage string, state string, detail string, contentHash string, cost float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage, state, detail, contentHash, Ftoa(cost)}
	status, _, err := c.callFields(ctx, opWfeFinish, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_finish", Status: status}
	}
	return nil
}

func (c *Client) WfeStopTree(ctx context.Context, workItemID string, max int) ([]string, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{workItemID, Itoa(max)}
	status, reply, err := c.callFields(ctx, opWfeStopTree, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "wfe_stop_tree", Status: status}
	}
	return reply, nil
}

func (c *Client) WfeReconcileOrphans(ctx context.Context, max int) ([]string, error) {
	if c == nil || c.caller == nil {
		return nil, ErrConfig
	}
	fields := []string{Itoa(max)}
	status, reply, err := c.callFields(ctx, opWfeReconcileOrphans, fields)
	if err != nil {
		return nil, err
	}
	if status != statusOK {
		return nil, &StatusError{Op: "wfe_reconcile_orphans", Status: status}
	}
	return reply, nil
}

func (c *Client) WfeParkBudgetTree(ctx context.Context, rootID string, completedItemID string, addedCost float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{rootID, completedItemID, Ftoa(addedCost)}
	status, _, err := c.callFields(ctx, opWfeParkBudgetTree, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_park_budget_tree", Status: status}
	}
	return nil
}

func (c *Client) WfeDeleteTree(ctx context.Context, workItemID string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID}
	status, _, err := c.callFields(ctx, opWfeDeleteTree, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_delete_tree", Status: status}
	}
	return nil
}

func (c *Client) WfeResolveGate(ctx context.Context, workItemID string, fromStage string, toStage string, decision string, contentHash string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, fromStage, toStage, decision, contentHash}
	status, _, err := c.callFields(ctx, opWfeResolveGate, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_resolve_gate", Status: status}
	}
	return nil
}

func (c *Client) WfeRejectGate(ctx context.Context, workItemID string, stage string, contentHash string) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage, contentHash}
	status, _, err := c.callFields(ctx, opWfeRejectGate, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_reject_gate", Status: status}
	}
	return nil
}

func (c *Client) WfeParkRunnerFailure(ctx context.Context, workItemID string, stage string, owner string, reason string, detail string, dispatched int, costKnown int, actual float64) error {
	if c == nil || c.caller == nil {
		return ErrConfig
	}
	fields := []string{workItemID, stage, owner, reason, detail, Itoa(dispatched), Itoa(costKnown), Ftoa(actual)}
	status, _, err := c.callFields(ctx, opWfeParkRunnerFailure, fields)
	if err != nil {
		return err
	}
	if status != statusOK {
		return &StatusError{Op: "wfe_park_runner_failure", Status: status}
	}
	return nil
}

func (c *Client) WfeRecoverLostReplay(ctx context.Context, workItemID string, stage string, owner string) (int, error) {
	if c == nil || c.caller == nil {
		return 0, ErrConfig
	}
	fields := []string{workItemID, stage, owner}
	status, reply, err := c.callFields(ctx, opWfeRecoverLostReplay, fields)
	if err != nil {
		return 0, err
	}
	if status != statusOK {
		return 0, &StatusError{Op: "wfe_recover_lost_replay", Status: status}
	}
	if len(reply) < 1 {
		return 0, ErrMalformed
	}
	return Atoi(reply[0])
}

func (c *Client) WfeRecordRequestedChanges(ctx context.Context, workItemID string, gate string, planStage string, planHash string, feedbackHash string, unresolved string, maxIterations int, maxIdentical int, cost float64) (WfeRecordRequestedChanges, error) {
	var out WfeRecordRequestedChanges
	if c == nil || c.caller == nil {
		return out, ErrConfig
	}
	fields := []string{workItemID, gate, planStage, planHash, feedbackHash, unresolved, Itoa(maxIterations), Itoa(maxIdentical), Ftoa(cost)}
	status, reply, err := c.callFields(ctx, opWfeRecordRequestedChanges, fields)
	if err != nil {
		return out, err
	}
	if status != statusOK {
		return out, &StatusError{Op: "wfe_record_requested_changes", Status: status}
	}
	if len(reply) != 4 {
		return out, fmt.Errorf("%w: wfe_record_requested_changes wants 4 fields, got %d", ErrMalformed, len(reply))
	}
	if value, parseErr := Atoi(reply[0]); parseErr == nil {
		out.Attempts = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atoi(reply[1]); parseErr == nil {
		out.IdenticalRepeats = value
	} else {
		return out, parseErr
	}
	if value, parseErr := Atoi(reply[2]); parseErr == nil {
		out.Parked = value
	} else {
		return out, parseErr
	}
	out.PauseReason = reply[3]
	return out, nil
}

func (c *Client) WfeClaimFrozenCreates(ctx context.Context, parentID string, workItemID string, createCount int, creates []WfeClaimFrozenCreatesItem) (WfeClaimFrozenCreates, error) {
	var out WfeClaimFrozenCreates
	if c == nil || c.caller == nil {
		return out, ErrConfig
	}
	if len(creates) > 64 {
		return out, fmt.Errorf("db1: wfe_claim_frozen_creates takes at most 64 creates, got %d", len(creates))
	}
	fields := make([]string, 0, 131)
	fields = append(fields, parentID)
	fields = append(fields, workItemID)
	for i := 0; i < 64; i++ {
		if i < len(creates) {
			fields = append(fields, creates[i].Path)
			fields = append(fields, creates[i].ContentHash)
		} else {
			fields = append(fields, "", "")
		}
	}
	fields = append(fields, Itoa(createCount))
	status, reply, err := c.callFields(ctx, opWfeClaimFrozenCreates, fields)
	if err != nil {
		return out, err
	}
	if status != statusOK {
		return out, &StatusError{Op: "wfe_claim_frozen_creates", Status: status}
	}
	if len(reply) != 3 {
		return out, fmt.Errorf("%w: wfe_claim_frozen_creates wants 3 fields, got %d", ErrMalformed, len(reply))
	}
	out.Path = reply[0]
	out.ExistingWorkItem = reply[1]
	out.ConflictingWorkItem = reply[2]
	return out, nil
}
