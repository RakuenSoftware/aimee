// Package db1 is the workflow engine's view of the DB1 store.
//
// It used to be the engine's SQLite driver: it opened $home/aimee.db -- the
// DB1 module's own file -- ran its own CREATE TABLE and ALTER ladder against it,
// and issued its own queries and recursive CTEs. Two processes with two schema
// authorities on one file, which is the arrangement the module doctrine exists
// to prevent and which scripts/validation/db1-module-wfe-coexistence.sh
// measured rather than argued about.
//
// Every one of those statements is now an operation the module serves, and this
// package is the mapping between the engine's vocabulary and that contract. The
// method set is unchanged, deliberately: the engine and its API surface were not
// the thing being fixed, and a port that also redesigned its callers would have
// been impossible to review against the behaviour it replaced.
//
// Two properties are worth stating because they are what make the mapping safe:
//
//   - No transaction spans two calls. Each of the engine's transactions became
//     ONE module operation, because a transaction assembled from separate calls
//     across a wire is not a transaction. Where a method below looks like it is
//     composing (WorkItemByProposal is a lookup then a fetch), the composition
//     is a read that was already two statements and needed no atomicity.
//   - Not-found stays sql.ErrNoRows. Callers in internal/api test for it, and a
//     port that quietly changed which error means "no such run" would change
//     which HTTP status a caller sees.
package db1

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"math"
	"strings"
	"time"

	wire "github.com/JBailes/aimee/server-go/db1"
)

// listCeiling bounds every list the engine asks for. The module refuses more
// than its own max_rows anyway; naming it here means a caller that wants "all
// of them" gets a number rather than a silently truncated page.
const listCeiling = 512

// Store is the engine's handle on the DB1 module.
type Store struct {
	client *wire.Client
}

// OpenBus seats the store on a module-bus client. There is no Open(path): the
// engine no longer has a path, which is the point of the change.
func OpenBus(client *wire.Client) (*Store, error) {
	if client == nil {
		return nil, errors.New("db1 store needs a module bus client")
	}
	return &Store{client: client}, nil
}

// Close releases nothing: the bus client's lifetime belongs to whoever attached
// it. Kept so callers that defer Close on a store keep compiling and keep
// meaning the same thing.
func (s *Store) Close() error { return nil }

func (s *Store) ready() error {
	if s == nil || s.client == nil {
		return errors.New("db1 store is not configured")
	}
	return nil
}

// notFound turns the module's "no row" into the error the API layer already
// tests for.
func notFound(what string) error {
	return fmt.Errorf("%s: %w", what, sql.ErrNoRows)
}

type DelegateJobMapping struct {
	ExecutionKey string
	JobID        int
}

const terminalCancellationBatchSize = 8

type CreateWorkItem struct {
	ID              string
	Repo            string
	ProposalPath    string
	WorkflowName    string
	WorkflowVersion string
	StartStage      string
	Mode            string
	Submitter       string
	ParentID        string
	SourcePath      string
	MaxCostUSD      float64
}

var ErrAdmissionFull = errors.New("trigger admission full")

type WorkItem struct {
	ID                string  `json:"id"`
	Repo              string  `json:"repo"`
	ProposalPath      string  `json:"-"`
	WorkflowName      string  `json:"workflow"`
	WorkflowVersion   string  `json:"version"`
	Stage             string  `json:"stage"`
	State             string  `json:"state"`
	Mode              string  `json:"mode"`
	PauseReason       string  `json:"pause_reason"`
	ContentHash       string  `json:"content_hash,omitempty"`
	PRRef             string  `json:"pr_ref"`
	Submitter         string  `json:"submitter"`
	CumulativeCostUSD float64 `json:"cum_cost_usd"`
	ReservedCostUSD   float64 `json:"reserved_cost_usd"`
	ReservationState  string  `json:"-"`
	MaxCostUSD        float64 `json:"work_item_max_cost_usd"`
	OverrideCount     int     `json:"override_count"`
	ParentID          string  `json:"parent_id,omitempty"`
	Worktree          string  `json:"worktree,omitempty"`
	SourcePath        string  `json:"-"`
	UpdatedAt         string  `json:"updated_at"`
}

type Event struct {
	ID          int64   `json:"id"`
	WorkItemID  string  `json:"work_item_id,omitempty"`
	Stage       string  `json:"stage"`
	Kind        string  `json:"kind"`
	Actor       string  `json:"actor"`
	Detail      string  `json:"detail"`
	ContentHash string  `json:"content_hash,omitempty"`
	CostUSD     float64 `json:"cost_usd"`
	CreatedAt   string  `json:"created_at"`
}

type BudgetReservation struct {
	RootID     string
	Amount     float64
	MaxUSD     float64
	Allowed    bool
	Busy       bool
	ReplayOnly bool
}

type ReviewOutcome struct {
	Attempts         int
	IdenticalRepeats int
	Parked           bool
	PauseReason      string
}

// workItemFromRow is written once against the generated row type. The engine's
// WorkItem and the module's reply carry the same values under different names,
// and doing this conversion in each caller is how two of them come to disagree.
func workItemFromRow(row wire.WorkItemGet) WorkItem {
	return WorkItem{
		ID:                row.WorkItemID,
		Repo:              row.Repo,
		ProposalPath:      row.ProposalPath,
		WorkflowName:      row.WorkflowName,
		WorkflowVersion:   row.WorkflowVersion,
		Stage:             row.CurrentStage,
		State:             row.State,
		Mode:              row.Mode,
		PauseReason:       row.PauseReason,
		ContentHash:       row.ContentHash,
		PRRef:             row.PRRef,
		Submitter:         row.Submitter,
		CumulativeCostUSD: row.CumCostUSD,
		ReservedCostUSD:   row.ReservedCostUSD,
		ReservationState:  row.ReservationState,
		MaxCostUSD:        row.WorkItemMaxCostUSD,
		OverrideCount:     row.OverrideCount,
		ParentID:          row.ParentID,
		Worktree:          row.Worktree,
		SourcePath:        row.SourcePath,
		UpdatedAt:         row.UpdatedAt,
	}
}

func workItemFromListRow(row wire.WorkItemList) WorkItem {
	return workItemFromRow(wire.WorkItemGet(row))
}

// ---------------------------------------------------------------------------
// Work items

func (s *Store) CreateWorkItem(ctx context.Context, in CreateWorkItem) error {
	return s.createWorkItem(ctx, in, 0)
}

// AdmitRoot creates a run only while fewer than cap root runs are active. The
// count and the insert are one operation in the module: counting here and
// inserting there is the check-then-act race the cap exists to prevent.
func (s *Store) AdmitRoot(ctx context.Context, in CreateWorkItem, cap int) error {
	return s.createWorkItem(ctx, in, cap)
}

func (s *Store) createWorkItem(ctx context.Context, in CreateWorkItem, cap int) error {
	if err := s.ready(); err != nil {
		return err
	}
	if in.ID == "" || in.ProposalPath == "" || in.WorkflowName == "" || in.StartStage == "" {
		return errors.New("work item id, proposal path, workflow, and start stage are required")
	}
	outcome, err := s.client.WfeCreateWorkItem(ctx, in.ID, in.Repo, in.ProposalPath, in.WorkflowName,
		in.WorkflowVersion, in.StartStage, in.Mode, in.Submitter, in.ParentID, in.SourcePath,
		in.MaxCostUSD, cap)
	if err != nil {
		return fmt.Errorf("create work item: %w", err)
	}
	switch outcome {
	case 1:
		return fmt.Errorf("%w (cap %d active root workflows)", ErrAdmissionFull, cap)
	case 2:
		// A stop won the race. Callers match on this text, and it is the
		// accurate description: the insert was refused because the parent had
		// already left the state that would have made the child legitimate.
		return errors.New("parent work item is not active")
	}
	return nil
}

func (s *Store) WorkItem(ctx context.Context, id string) (WorkItem, error) {
	if err := s.ready(); err != nil {
		return WorkItem{}, err
	}
	row, err := s.client.WorkItemGet(ctx, id)
	if err != nil {
		return WorkItem{}, err
	}
	if row.WorkItemID == "" {
		return WorkItem{}, notFound("work item " + id)
	}
	return workItemFromRow(row), nil
}

func (s *Store) WorkItemByProposal(ctx context.Context, repo, proposalPath string) (WorkItem, error) {
	if err := s.ready(); err != nil {
		return WorkItem{}, err
	}
	id, err := s.client.WorkItemIDByProposal(ctx, repo, proposalPath)
	if err != nil {
		return WorkItem{}, err
	}
	if id == "" {
		return WorkItem{}, notFound("work item for proposal " + proposalPath)
	}
	return s.WorkItem(ctx, id)
}

// GitProposalIdentity is the proposal path a git-sourced run is filed under.
// The blob hash alone is not enough: the same content built by a different
// workflow, or in a different mode, is a different run.
func GitProposalIdentity(proposalHash, workflow, mode string) string {
	return fmt.Sprintf("git:%s:%s:%s", proposalHash, workflow, mode)
}

// WorkItemByGitProposal finds both the current blob-based trigger identity and
// the older commit-qualified identity. A proposal that has not changed must not
// be filed again merely because its watched branch advanced -- which is why the
// legacy form is matched by SUFFIX: the old identity carried a commit in front
// of the part that actually identifies the work.
func (s *Store) WorkItemByGitProposal(ctx context.Context, repo, proposalHash, workflow,
	mode string) (WorkItem, error) {
	if err := s.ready(); err != nil {
		return WorkItem{}, err
	}
	identity := GitProposalIdentity(proposalHash, workflow, mode)
	legacySuffix := fmt.Sprintf(":%s:%s:%s", proposalHash, workflow, mode)
	id, err := s.client.WfeWorkItemIDByGitProposal(ctx, repo, identity, legacySuffix)
	if err != nil {
		return WorkItem{}, err
	}
	if id == "" {
		return WorkItem{}, notFound("work item for git proposal " + identity)
	}
	return s.WorkItem(ctx, id)
}

func (s *Store) WorkItems(ctx context.Context) ([]WorkItem, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	rows, err := s.client.WorkItemList(ctx, listCeiling)
	if err != nil {
		return nil, err
	}
	items := make([]WorkItem, 0, len(rows))
	for _, row := range rows {
		items = append(items, workItemFromListRow(row))
	}
	return items, nil
}

func (s *Store) SetWorktree(ctx context.Context, workItemID, worktree string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WorkItemSetWorktree(ctx, workItemID, worktree)
}

func (s *Store) SetPRRef(ctx context.Context, workItemID, prRef string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WorkItemSetPRRef(ctx, workItemID, prRef)
}

func (s *Store) Children(ctx context.Context, parentID string) ([]WorkItem, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	ids, err := s.client.WfeChildrenList(ctx, parentID, listCeiling)
	if err != nil {
		return nil, err
	}
	children := make([]WorkItem, 0, len(ids))
	for _, id := range ids {
		child, err := s.WorkItem(ctx, id)
		if err != nil {
			// A child that vanished between the list and the fetch is not an
			// error: the caller asked what the children are, and that one is no
			// longer among them.
			if errors.Is(err, sql.ErrNoRows) {
				continue
			}
			return nil, err
		}
		children = append(children, child)
	}
	return children, nil
}

func (s *Store) ActiveRootCount(ctx context.Context) (int, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeActiveRootCount(ctx)
}

func (s *Store) DescendantIDs(ctx context.Context, workItemID string) ([]string, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	return s.client.WfeDescendantIDs(ctx, workItemID, listCeiling)
}

func (s *Store) Delete(ctx context.Context, workItemID string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeDeleteTree(ctx, workItemID)
}

// ---------------------------------------------------------------------------
// Events

func (s *Store) Events(ctx context.Context, workItemID string, after int64, limit int) ([]Event, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	if limit <= 0 || limit > listCeiling {
		limit = listCeiling
	}
	// The module returns a run's events oldest-first; `after` is a cursor the
	// caller advances. Filtering here rather than in the operation keeps the
	// operation the same one the daemon's own API uses.
	rows, err := s.client.LifecycleEventList(ctx, workItemID, listCeiling)
	if err != nil {
		return nil, err
	}
	events := make([]Event, 0, len(rows))
	for _, row := range rows {
		if row.ID <= after {
			continue
		}
		events = append(events, Event{
			ID:          row.ID,
			WorkItemID:  workItemID,
			Stage:       row.Stage,
			Kind:        row.Kind,
			Actor:       row.Actor,
			Detail:      row.Detail,
			ContentHash: row.ContentHash,
			CostUSD:     row.CostUSD,
			CreatedAt:   row.CreatedAt,
		})
		if len(events) >= limit {
			break
		}
	}
	return events, nil
}

func (s *Store) ExecutedTurnCount(ctx context.Context, workItemID string) (int, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeExecutedTurnCount(ctx, workItemID)
}

func (s *Store) StageLoopCount(ctx context.Context, workItemID, stage string) (int, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeStageLoopCount(ctx, workItemID, stage)
}

func (s *Store) StageAttemptCount(ctx context.Context, workItemID, stage string) (int, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.StageAttemptGet(ctx, workItemID, stage)
}

func (s *Store) LatestStageRetryDetail(ctx context.Context, workItemID, stage string) (string, error) {
	if err := s.ready(); err != nil {
		return "", err
	}
	return s.client.WfeLatestStageRetryDetail(ctx, workItemID, stage)
}

func (s *Store) RunnerFailuresSinceProgress(ctx context.Context, workItemID, stage string) (int, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeRunnerFailuresSinceProgress(ctx, workItemID, stage)
}

func (s *Store) CapacityWaitsSinceProgress(ctx context.Context, workItemID, stage string) (int, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeCapacityWaitsSinceProgress(ctx, workItemID, stage)
}

// ---------------------------------------------------------------------------
// Budget

func (s *Store) WorkflowBudget(ctx context.Context, workItemID string) (rootID string, spent,
	max float64, err error) {
	if err := s.ready(); err != nil {
		return "", 0, 0, err
	}
	totals, err := s.client.WfeBudgetTotals(ctx, workItemID)
	if err != nil {
		return "", 0, 0, err
	}
	return totals.RootID, totals.Spent, totals.MaxUSD, nil
}

func (s *Store) ReserveWorkflowBudget(ctx context.Context, workItemID,
	owner string) (BudgetReservation, error) {
	if err := s.ready(); err != nil {
		return BudgetReservation{}, err
	}
	if owner == "" {
		return BudgetReservation{}, errors.New("workflow budget reservation owner is required")
	}
	// The retry loop that used to be here is inside the operation now: a lost
	// race is discovered by an UPDATE matching zero rows INSIDE the module's
	// transaction, and only the process holding that transaction can decide to
	// try again.
	got, err := s.client.WfeBudgetReserve(ctx, workItemID, owner)
	if err != nil {
		return BudgetReservation{}, err
	}
	return BudgetReservation{
		RootID:     got.RootID,
		Amount:     got.Amount,
		MaxUSD:     got.MaxUSD,
		Allowed:    got.Allowed == 1,
		Busy:       got.Busy == 1,
		ReplayOnly: got.ReplayOnly == 1,
	}, nil
}

func (s *Store) ReconcileWorkflowBudget(ctx context.Context, workItemID, owner string,
	actual float64) (bool, error) {
	if err := s.ready(); err != nil {
		return false, err
	}
	if actual < 0 || math.IsNaN(actual) || math.IsInf(actual, 0) {
		return false, errors.New("workflow cost must be finite and non-negative")
	}
	// The module answers with whether the tree can AFFORD this cost, not with
	// whether it wrote it. The caller parks the tree when the answer is no, so
	// conflating the two would let an overspend through as "already recorded".
	allowed, err := s.client.WfeBudgetReconcile(ctx, workItemID, owner, actual)
	if err != nil {
		return false, err
	}
	return allowed == 1, nil
}

func (s *Store) ReleaseWorkflowBudget(ctx context.Context, workItemID, owner string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeBudgetRelease(ctx, workItemID, owner)
}

func (s *Store) HeartbeatWorkflowBudget(ctx context.Context, workItemID, owner string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeBudgetHeartbeat(ctx, workItemID, owner)
}

func (s *Store) ParkBudgetTree(ctx context.Context, rootID, completedItemID string,
	addedCost float64) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeParkBudgetTree(ctx, rootID, completedItemID, addedCost)
}

func (s *Store) ParkRunnerFailure(ctx context.Context, workItemID, stage, owner, reason,
	detail string, dispatched, costKnown bool, actual float64) error {
	if err := s.ready(); err != nil {
		return err
	}
	if actual < 0 || math.IsNaN(actual) || math.IsInf(actual, 0) {
		return errors.New("runner failure cost must be finite and non-negative")
	}
	// The store refuses this when its UPDATE matches no row, which has one
	// cause: the reservation this invocation held is no longer the one on the
	// row -- a sweep or another transition moved it while the runner was still
	// out. That is a recognised outcome, not a fault, and it used to say so.
	// Crossing the module boundary it became "refused with status 4", which
	// tells an operator reading the log nothing at all, so name it again here.
	if err := s.client.WfeParkRunnerFailure(ctx, workItemID, stage, owner, reason, detail,
		boolToInt(dispatched), boolToInt(costKnown), actual); err != nil {
		return fmt.Errorf("runner failure reservation changed concurrently "+
			"(work item %s, stage %s): %w", workItemID, stage, err)
	}
	return nil
}

func (s *Store) RecoverLostReplay(ctx context.Context, workItemID, stage,
	owner string) (redispatch bool, err error) {
	if err := s.ready(); err != nil {
		return false, err
	}
	again, err := s.client.WfeRecoverLostReplay(ctx, workItemID, stage, owner)
	if err != nil {
		return false, err
	}
	return again == 1, nil
}

func boolToInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
// Transitions

func (s *Store) Move(ctx context.Context, workItemID, fromStage, toStage, kind, detail,
	contentHash string, costUSD float64) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeMove(ctx, workItemID, fromStage, toStage, kind, detail, contentHash, costUSD)
}

func (s *Store) RecordRetry(ctx context.Context, workItemID, stage, toStage, detail string,
	maxAttempts int, costUSD float64) (bool, error) {
	if err := s.ready(); err != nil {
		return false, err
	}
	if maxAttempts < 1 {
		return false, errors.New("retry limit must be positive")
	}
	parked, err := s.client.WfeRecordRetry(ctx, workItemID, stage, toStage, detail, maxAttempts,
		costUSD)
	if err != nil {
		return false, err
	}
	return parked == 1, nil
}

// Park records the stable pause reason as both item state and event detail.
// Call ParkWithDetail when an operator-safe diagnostic should accompany it.
func (s *Store) Park(ctx context.Context, workItemID, stage, reason string, costUSD float64) error {
	return s.ParkWithDetail(ctx, workItemID, stage, reason, reason, costUSD)
}

func (s *Store) ParkWithDetail(ctx context.Context, workItemID, stage, reason, detail string,
	costUSD float64) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeParkWithDetail(ctx, workItemID, stage, reason, detail, costUSD)
}

func (s *Store) Resume(ctx context.Context, workItemID string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeResume(ctx, workItemID)
}

func (s *Store) Pause(ctx context.Context, workItemID string) error {
	item, err := s.WorkItem(ctx, workItemID)
	if err != nil {
		return err
	}
	return s.Park(ctx, workItemID, item.Stage, "manual", 0)
}

func (s *Store) Stop(ctx context.Context, workItemID string) error {
	_, err := s.StopTree(ctx, workItemID)
	return err
}

func (s *Store) StopTree(ctx context.Context, workItemID string) ([]string, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	return s.client.WfeStopTree(ctx, workItemID, listCeiling)
}

func (s *Store) ReconcileOrphanedDescendants(ctx context.Context) ([]string, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	return s.client.WfeReconcileOrphans(ctx, listCeiling)
}

func (s *Store) ResolveGate(ctx context.Context, workItemID, fromStage, toStage, decision,
	contentHash string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeResolveGate(ctx, workItemID, fromStage, toStage, decision, contentHash)
}

func (s *Store) RejectGate(ctx context.Context, workItemID, stage, contentHash string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeRejectGate(ctx, workItemID, stage, contentHash)
}

func (s *Store) Finish(ctx context.Context, workItemID, stage, state, detail,
	contentHash string, costUSD float64) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeFinish(ctx, workItemID, stage, state, detail, contentHash, costUSD)
}

func (s *Store) RecordRequestedChanges(ctx context.Context, workItemID, gate, planStage,
	planHash, feedbackHash, unresolved string, maxIterations, maxIdentical int,
	costUSD float64) (ReviewOutcome, error) {
	if err := s.ready(); err != nil {
		return ReviewOutcome{}, err
	}
	if maxIterations < 1 || maxIdentical < 1 {
		return ReviewOutcome{}, errors.New("review limits must be positive")
	}
	got, err := s.client.WfeRecordRequestedChanges(ctx, workItemID, gate, planStage, planHash,
		feedbackHash, unresolved, maxIterations, maxIdentical, costUSD)
	if err != nil {
		return ReviewOutcome{}, err
	}
	return ReviewOutcome{
		Attempts:         got.Attempts,
		IdenticalRepeats: got.IdenticalRepeats,
		Parked:           got.Parked == 1,
		PauseReason:      got.PauseReason,
	}, nil
}

// ---------------------------------------------------------------------------
// Sweeps

func (s *Store) ResumeTransient(ctx context.Context, reason string,
	olderThan time.Duration) (int64, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeResumeTransient(ctx, reason, int(olderThan.Seconds()))
}

func (s *Store) ResumeReadyParents(ctx context.Context) (int64, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeResumeReadyParents(ctx)
}

func (s *Store) ResumeWallCaps(ctx context.Context, maxResumes int) (int64, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeResumeWallCaps(ctx, maxResumes)
}

func (s *Store) AbandonExhaustedWallCaps(ctx context.Context, maxResumes int,
	grace time.Duration) (int64, error) {
	if err := s.ready(); err != nil {
		return 0, err
	}
	return s.client.WfeAbandonExhaustedWallCaps(ctx, maxResumes, int(grace.Seconds()))
}

// ---------------------------------------------------------------------------
// Delegate jobs

func (s *Store) SaveWorkflowDelegateJob(ctx context.Context, key, workItemID string, id int,
	participant string) error {
	if err := s.ready(); err != nil {
		return err
	}
	return s.client.WfeDelegateJobSave(ctx, key, workItemID, int64(id), participant)
}

// TerminalDelegateJobs returns durable resource-plane jobs owned by workflow
// items that can no longer execute, and claims them in the same operation.
// Retaining the mapping until remote cancellation is acknowledged makes a crash
// between lifecycle commit and cancellation recoverable on the next fill.
func (s *Store) TerminalDelegateJobs(ctx context.Context) ([]DelegateJobMapping, error) {
	if err := s.ready(); err != nil {
		return nil, err
	}
	rows, err := s.client.WfeDelegateJobsTerminalClaim(ctx, terminalCancellationBatchSize)
	if err != nil {
		return nil, err
	}
	out := make([]DelegateJobMapping, 0, len(rows))
	for _, row := range rows {
		out = append(out, DelegateJobMapping{ExecutionKey: row.ExecutionKey, JobID: int(row.JobID)})
	}
	return out, nil
}

// isSQLiteContention is retained because callers outside this package still
// classify errors with it. Contention is handled inside the module now -- the
// retry loop lives next to the transaction -- so this only ever answers about
// an error the module chose to report.
func isSQLiteContention(err error) bool {
	if err == nil {
		return false
	}
	message := strings.ToLower(err.Error())
	return strings.Contains(message, "database is locked") ||
		strings.Contains(message, "database is busy") ||
		strings.Contains(message, "sqlite_busy")
}
