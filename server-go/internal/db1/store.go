package db1

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"path/filepath"
	"time"

	_ "modernc.org/sqlite"
)

type Store struct {
	db *sql.DB
}

func Open(path string) (*Store, error) {
	if path == "" {
		return nil, errors.New("DB1 path is required")
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, fmt.Errorf("resolve DB1 path: %w", err)
	}
	dsn := (&url.URL{Scheme: "file", Path: abs, RawQuery: "_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=foreign_keys(1)"}).String()
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open DB1: %w", err)
	}
	db.SetMaxOpenConns(1)
	store := &Store{db: db}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := db.PingContext(ctx); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("ping DB1: %w", err)
	}
	if err := store.migrate(ctx); err != nil {
		_ = db.Close()
		return nil, err
	}
	return store, nil
}

func (s *Store) Close() error { return s.db.Close() }

func (s *Store) migrate(ctx context.Context) error {
	const schema = `
CREATE TABLE IF NOT EXISTS lifecycle_work_item (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  work_item_id TEXT NOT NULL UNIQUE,
  repo TEXT NOT NULL DEFAULT '',
  proposal_path TEXT NOT NULL DEFAULT '',
  workflow_name TEXT NOT NULL DEFAULT 'build',
  workflow_version TEXT NOT NULL DEFAULT '',
  current_stage TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL DEFAULT 'active',
  mode TEXT NOT NULL DEFAULT 'interactive',
  pause_reason TEXT NOT NULL DEFAULT '',
  paused_state TEXT NOT NULL DEFAULT '',
  content_hash TEXT NOT NULL DEFAULT '',
  pr_ref TEXT NOT NULL DEFAULT '',
  worktree TEXT NOT NULL DEFAULT '',
  submitter TEXT NOT NULL DEFAULT '',
  cum_cost_usd REAL NOT NULL DEFAULT 0,
  work_item_max_cost_usd REAL NOT NULL DEFAULT 0,
  override_count INTEGER NOT NULL DEFAULT 0,
  parent_id TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now')),
  UNIQUE(repo, proposal_path)
);
CREATE TABLE IF NOT EXISTS lifecycle_event (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  work_item_id TEXT NOT NULL,
  stage TEXT NOT NULL DEFAULT '',
  kind TEXT NOT NULL DEFAULT '',
  actor TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  content_hash TEXT NOT NULL DEFAULT '',
  cost_usd REAL NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_lifecycle_event_wi ON lifecycle_event(work_item_id);
CREATE TABLE IF NOT EXISTS lifecycle_stage_attempt (
  work_item_id TEXT NOT NULL,
  stage TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (work_item_id, stage)
);
CREATE TABLE IF NOT EXISTS wfe_convergence (
  work_item_id TEXT NOT NULL,
  gate TEXT NOT NULL,
  artifact_hash TEXT NOT NULL,
  feedback_hash TEXT NOT NULL,
  identical_repeats INTEGER NOT NULL,
  updated_at TEXT NOT NULL DEFAULT (datetime('now')),
  PRIMARY KEY (work_item_id, gate)
);`
	if _, err := s.db.ExecContext(ctx, schema); err != nil {
		return fmt.Errorf("migrate DB1 WFE schema: %w", err)
	}
	return nil
}

type CreateWorkItem struct {
	ID              string
	Repo            string
	ProposalPath    string
	WorkflowName    string
	WorkflowVersion string
	StartStage      string
	Mode            string
	Submitter       string
}

func (s *Store) CreateWorkItem(ctx context.Context, in CreateWorkItem) error {
	if in.ID == "" || in.ProposalPath == "" || in.WorkflowName == "" || in.StartStage == "" {
		return errors.New("work item id, proposal path, workflow, and start stage are required")
	}
	if in.Mode == "" {
		in.Mode = "autonomous"
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin create work item: %w", err)
	}
	defer tx.Rollback()
	_, err = tx.ExecContext(ctx, `
INSERT INTO lifecycle_work_item
  (work_item_id, repo, proposal_path, workflow_name, workflow_version,
   current_stage, mode, submitter)
VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, in.ID, in.Repo, in.ProposalPath, in.WorkflowName,
		in.WorkflowVersion, in.StartStage, in.Mode, in.Submitter)
	if err != nil {
		return fmt.Errorf("insert work item: %w", err)
	}
	_, err = tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash)
VALUES (?, ?, 'create', ?, ?, ?)`, in.ID, in.StartStage, in.Submitter, in.WorkflowName,
		in.WorkflowVersion)
	if err != nil {
		return fmt.Errorf("insert create event: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit create work item: %w", err)
	}
	return nil
}

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
	MaxCostUSD        float64 `json:"work_item_max_cost_usd"`
	OverrideCount     int     `json:"override_count"`
	UpdatedAt         string  `json:"updated_at"`
}

func (s *Store) WorkItem(ctx context.Context, id string) (WorkItem, error) {
	var item WorkItem
	err := s.db.QueryRowContext(ctx, `
SELECT work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage,
       state, mode, pause_reason, content_hash, pr_ref, submitter, cum_cost_usd,
       work_item_max_cost_usd, override_count, updated_at
FROM lifecycle_work_item WHERE work_item_id = ?`, id).Scan(
		&item.ID, &item.Repo, &item.ProposalPath, &item.WorkflowName, &item.WorkflowVersion,
		&item.Stage, &item.State, &item.Mode, &item.PauseReason, &item.ContentHash, &item.PRRef,
		&item.Submitter, &item.CumulativeCostUSD, &item.MaxCostUSD, &item.OverrideCount,
		&item.UpdatedAt)
	if err != nil {
		return WorkItem{}, fmt.Errorf("get work item: %w", err)
	}
	return item, nil
}

func (s *Store) WorkItemByProposal(ctx context.Context, repo, proposalPath string) (WorkItem, error) {
	var id string
	if err := s.db.QueryRowContext(ctx, `
SELECT work_item_id FROM lifecycle_work_item WHERE repo = ? AND proposal_path = ?`,
		repo, proposalPath).Scan(&id); err != nil {
		return WorkItem{}, fmt.Errorf("find work item by proposal: %w", err)
	}
	return s.WorkItem(ctx, id)
}

func (s *Store) WorkItems(ctx context.Context) ([]WorkItem, error) {
	rows, err := s.db.QueryContext(ctx, `
SELECT work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage,
       state, mode, pause_reason, content_hash, pr_ref, submitter, cum_cost_usd,
       work_item_max_cost_usd, override_count, updated_at
FROM lifecycle_work_item ORDER BY id DESC`)
	if err != nil {
		return nil, fmt.Errorf("list work items: %w", err)
	}
	defer rows.Close()
	var items []WorkItem
	for rows.Next() {
		var item WorkItem
		if err := rows.Scan(&item.ID, &item.Repo, &item.ProposalPath, &item.WorkflowName,
			&item.WorkflowVersion, &item.Stage, &item.State, &item.Mode, &item.PauseReason,
			&item.ContentHash, &item.PRRef, &item.Submitter, &item.CumulativeCostUSD,
			&item.MaxCostUSD, &item.OverrideCount, &item.UpdatedAt); err != nil {
			return nil, fmt.Errorf("scan work item: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate work items: %w", err)
	}
	return items, nil
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

func (s *Store) Events(ctx context.Context, workItemID string, after int64, limit int) ([]Event, error) {
	if limit < 1 {
		limit = 200
	}
	rows, err := s.db.QueryContext(ctx, `
SELECT id, work_item_id, stage, kind, actor, detail, content_hash, cost_usd, created_at
FROM lifecycle_event WHERE work_item_id = ? AND id > ? ORDER BY id ASC LIMIT ?`,
		workItemID, after, limit)
	if err != nil {
		return nil, fmt.Errorf("list lifecycle events: %w", err)
	}
	defer rows.Close()
	var events []Event
	for rows.Next() {
		var event Event
		if err := rows.Scan(&event.ID, &event.WorkItemID, &event.Stage, &event.Kind,
			&event.Actor, &event.Detail, &event.ContentHash, &event.CostUSD,
			&event.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan lifecycle event: %w", err)
		}
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate lifecycle events: %w", err)
	}
	return events, nil
}

func (s *Store) Move(ctx context.Context, workItemID, fromStage, toStage, kind, detail,
	contentHash string, costUSD float64) error {
	if workItemID == "" || fromStage == "" || toStage == "" || kind == "" {
		return errors.New("complete stage transition coordinates are required")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin stage transition: %w", err)
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET current_stage=?, content_hash=?, pause_reason='', paused_state='',
    cum_cost_usd=cum_cost_usd+?, updated_at=datetime('now')
WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''`,
		toStage, contentHash, costUSD, workItemID, fromStage)
	if err != nil {
		return fmt.Errorf("move work item: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("read transition result: %w", err)
	}
	if changed != 1 {
		return errors.New("work item changed concurrently or is not runnable")
	}
	if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
VALUES (?, ?, ?, 'go-wfe', ?, ?, ?)`, workItemID, fromStage, kind, detail, contentHash,
		costUSD); err != nil {
		return fmt.Errorf("record stage transition: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit stage transition: %w", err)
	}
	return nil
}

func (s *Store) Park(ctx context.Context, workItemID, stage, reason string, costUSD float64) error {
	if workItemID == "" || stage == "" || reason == "" {
		return errors.New("work item, stage, and pause reason are required")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin park transition: %w", err)
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET pause_reason=?, paused_state=?, cum_cost_usd=cum_cost_usd+?, updated_at=datetime('now')
WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''`,
		reason, stage, costUSD, workItemID, stage)
	if err != nil {
		return fmt.Errorf("park work item: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil || changed != 1 {
		return errors.New("work item changed concurrently or is not runnable")
	}
	if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, cost_usd)
VALUES (?, ?, 'pause', 'go-wfe', ?, ?)`, workItemID, stage, reason, costUSD); err != nil {
		return fmt.Errorf("record park transition: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit park transition: %w", err)
	}
	return nil
}

// ResumeTransient clears a scheduler-owned transient pause after a bounded
// backoff. It never resumes human/operator/convergence parks.
func (s *Store) ResumeTransient(ctx context.Context, reason string, olderThan time.Duration) (int64, error) {
	if reason == "" || olderThan < 0 {
		return 0, errors.New("transient reason and non-negative backoff are required")
	}
	cutoff := time.Now().UTC().Add(-olderThan).Format("2006-01-02 15:04:05")
	result, err := s.db.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET pause_reason='', paused_state='', updated_at=datetime('now')
WHERE state='active' AND pause_reason=? AND updated_at <= ?`, reason, cutoff)
	if err != nil {
		return 0, fmt.Errorf("resume transient workflows: %w", err)
	}
	count, err := result.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("read transient resume count: %w", err)
	}
	return count, nil
}

func (s *Store) Finish(ctx context.Context, workItemID, stage, state, detail,
	contentHash string, costUSD float64) error {
	if state != "accepted" && state != "rejected" && state != "stopped" {
		return fmt.Errorf("invalid terminal state %q", state)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin terminal transition: %w", err)
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET state=?, pause_reason='', paused_state='', content_hash=?,
    cum_cost_usd=cum_cost_usd+?, updated_at=datetime('now')
WHERE work_item_id=? AND current_stage=? AND state='active'`,
		state, contentHash, costUSD, workItemID, stage)
	if err != nil {
		return fmt.Errorf("finish work item: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil || changed != 1 {
		return errors.New("work item changed concurrently or is not active")
	}
	if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
VALUES (?, ?, 'terminal', 'go-wfe', ?, ?, ?)`, workItemID, stage, detail, contentHash,
		costUSD); err != nil {
		return fmt.Errorf("record terminal transition: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit terminal transition: %w", err)
	}
	return nil
}

type ReviewOutcome struct {
	Attempts         int
	IdenticalRepeats int
	Parked           bool
	PauseReason      string
}

// RecordRequestedChanges atomically records a plan-gate rejection. A retry cap
// is a recoverable park, never terminal abandonment. Repeating an identical
// plan+feedback pair parks early because no information is changing.
func (s *Store) RecordRequestedChanges(ctx context.Context, workItemID, gate, planStage,
	planHash, feedbackHash string, maxIterations, maxIdentical int) (ReviewOutcome, error) {
	if workItemID == "" || gate == "" || planStage == "" || planHash == "" || feedbackHash == "" {
		return ReviewOutcome{}, errors.New("complete review transition coordinates are required")
	}
	if maxIterations < 1 || maxIdentical < 1 {
		return ReviewOutcome{}, errors.New("review limits must be positive")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ReviewOutcome{}, fmt.Errorf("begin review transition: %w", err)
	}
	defer tx.Rollback()

	var state string
	if err := tx.QueryRowContext(ctx,
		"SELECT state FROM lifecycle_work_item WHERE work_item_id = ?", workItemID).Scan(&state); err != nil {
		return ReviewOutcome{}, fmt.Errorf("load review work item: %w", err)
	}
	if state != "active" {
		return ReviewOutcome{}, fmt.Errorf("work item is %s, not active", state)
	}

	if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_stage_attempt (work_item_id, stage, attempts) VALUES (?, ?, 1)
ON CONFLICT(work_item_id, stage) DO UPDATE SET attempts = attempts + 1`, workItemID, gate); err != nil {
		return ReviewOutcome{}, fmt.Errorf("increment gate attempts: %w", err)
	}
	var attempts int
	if err := tx.QueryRowContext(ctx, `
SELECT attempts FROM lifecycle_stage_attempt WHERE work_item_id = ? AND stage = ?`,
		workItemID, gate).Scan(&attempts); err != nil {
		return ReviewOutcome{}, fmt.Errorf("read gate attempts: %w", err)
	}

	repeats := 1
	var oldPlanHash, oldFeedbackHash string
	var oldRepeats int
	err = tx.QueryRowContext(ctx, `
SELECT artifact_hash, feedback_hash, identical_repeats
FROM wfe_convergence WHERE work_item_id = ? AND gate = ?`, workItemID, gate).Scan(
		&oldPlanHash, &oldFeedbackHash, &oldRepeats)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return ReviewOutcome{}, fmt.Errorf("read convergence observation: %w", err)
	}
	if err == nil && oldPlanHash == planHash && oldFeedbackHash == feedbackHash {
		repeats = oldRepeats + 1
	}
	if _, err := tx.ExecContext(ctx, `
INSERT INTO wfe_convergence
  (work_item_id, gate, artifact_hash, feedback_hash, identical_repeats, updated_at)
VALUES (?, ?, ?, ?, ?, datetime('now'))
ON CONFLICT(work_item_id, gate) DO UPDATE SET
  artifact_hash=excluded.artifact_hash,
  feedback_hash=excluded.feedback_hash,
  identical_repeats=excluded.identical_repeats,
  updated_at=datetime('now')`, workItemID, gate, planHash, feedbackHash, repeats); err != nil {
		return ReviewOutcome{}, fmt.Errorf("write convergence observation: %w", err)
	}

	out := ReviewOutcome{Attempts: attempts, IdenticalRepeats: repeats}
	if repeats >= maxIdentical {
		out.Parked = true
		out.PauseReason = "convergence_no_progress"
	} else if attempts >= maxIterations {
		out.Parked = true
		out.PauseReason = "convergence_limit"
	}
	if out.Parked {
		if _, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET current_stage=?, pause_reason=?, paused_state=?, content_hash=?, updated_at=datetime('now')
WHERE work_item_id=?`, planStage, out.PauseReason, gate, planHash, workItemID); err != nil {
			return ReviewOutcome{}, fmt.Errorf("park non-converging work item: %w", err)
		}
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash)
VALUES (?, ?, 'pause', 'go-wfe', ?, ?)`, workItemID, gate, out.PauseReason, planHash); err != nil {
			return ReviewOutcome{}, fmt.Errorf("record convergence park: %w", err)
		}
	} else {
		if _, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET current_stage=?, pause_reason='', paused_state='', content_hash=?, updated_at=datetime('now')
WHERE work_item_id=?`, planStage, planHash, workItemID); err != nil {
			return ReviewOutcome{}, fmt.Errorf("route work item to plan refinement: %w", err)
		}
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash)
VALUES (?, ?, 'loop', 'go-wfe', 'requested_changes', ?)`, workItemID, gate, planHash); err != nil {
			return ReviewOutcome{}, fmt.Errorf("record review loop: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return ReviewOutcome{}, fmt.Errorf("commit review transition: %w", err)
	}
	return out, nil
}
