package db1

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"math"
	"net/url"
	"path/filepath"
	"sort"
	"strings"
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
	dsn := (&url.URL{Scheme: "file", Path: abs, RawQuery: "_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=foreign_keys(1)&_txlock=immediate"}).String()
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
  reserved_cost_usd REAL NOT NULL DEFAULT 0,
  reservation_state TEXT NOT NULL DEFAULT '',
  reservation_owner TEXT NOT NULL DEFAULT '',
  reservation_lease_until TEXT NOT NULL DEFAULT '',
  work_item_max_cost_usd REAL NOT NULL DEFAULT 0,
  override_count INTEGER NOT NULL DEFAULT 0,
  parent_id TEXT NOT NULL DEFAULT '',
  source_path TEXT NOT NULL DEFAULT '',
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
);
CREATE TABLE IF NOT EXISTS wfe_frozen_create (
  parent_id TEXT NOT NULL,
  path TEXT NOT NULL,
  work_item_id TEXT NOT NULL,
  content_hash TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now')),
  PRIMARY KEY (parent_id, path, work_item_id)
);
CREATE INDEX IF NOT EXISTS idx_wfe_frozen_create_item ON wfe_frozen_create(work_item_id);
CREATE TABLE IF NOT EXISTS lifecycle_delegate_job (
  execution_key TEXT PRIMARY KEY,
  job_id INTEGER NOT NULL,
  work_item_id TEXT NOT NULL DEFAULT '',
  participant_token TEXT NOT NULL DEFAULT '',
  cancel_attempts INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);`
	if _, err := s.db.ExecContext(ctx, schema); err != nil {
		return fmt.Errorf("migrate DB1 WFE schema: %w", err)
	}
	for _, migration := range []struct{ column, ddl string }{
		{"parent_id", `ALTER TABLE lifecycle_work_item ADD COLUMN parent_id TEXT NOT NULL DEFAULT ''`},
		{"worktree", `ALTER TABLE lifecycle_work_item ADD COLUMN worktree TEXT NOT NULL DEFAULT ''`},
		{"source_path", `ALTER TABLE lifecycle_work_item ADD COLUMN source_path TEXT NOT NULL DEFAULT ''`},
		{"work_item_max_cost_usd", `ALTER TABLE lifecycle_work_item ADD COLUMN work_item_max_cost_usd REAL NOT NULL DEFAULT 0`},
		{"reserved_cost_usd", `ALTER TABLE lifecycle_work_item ADD COLUMN reserved_cost_usd REAL NOT NULL DEFAULT 0`},
		{"reservation_state", `ALTER TABLE lifecycle_work_item ADD COLUMN reservation_state TEXT NOT NULL DEFAULT ''`},
		{"reservation_owner", `ALTER TABLE lifecycle_work_item ADD COLUMN reservation_owner TEXT NOT NULL DEFAULT ''`},
		{"reservation_lease_until", `ALTER TABLE lifecycle_work_item ADD COLUMN reservation_lease_until TEXT NOT NULL DEFAULT ''`},
	} {
		has, err := s.hasWorkItemColumn(ctx, migration.column)
		if err != nil {
			return err
		}
		if !has {
			if _, err := s.db.ExecContext(ctx, migration.ddl); err != nil {
				return fmt.Errorf("add DB1 column %s: %w", migration.column, err)
			}
		}
	}
	if _, err := s.db.ExecContext(ctx, `CREATE INDEX IF NOT EXISTS idx_lifecycle_work_item_parent_state ON lifecycle_work_item(parent_id, state)`); err != nil {
		return fmt.Errorf("index DB1 workflow parent state: %w", err)
	}
	_, _ = s.db.ExecContext(ctx, `ALTER TABLE lifecycle_delegate_job ADD COLUMN participant_token TEXT NOT NULL DEFAULT ''`)
	_, _ = s.db.ExecContext(ctx, `ALTER TABLE lifecycle_delegate_job ADD COLUMN work_item_id TEXT NOT NULL DEFAULT ''`)
	_, _ = s.db.ExecContext(ctx, `ALTER TABLE lifecycle_delegate_job ADD COLUMN cancel_attempts INTEGER NOT NULL DEFAULT 0`)
	// Recover structural ownership for mappings written before work_item_id was
	// stored explicitly. The longest prefix prevents one workflow ID from
	// claiming a descendant's mapping.
	_, _ = s.db.ExecContext(ctx, `UPDATE lifecycle_delegate_job
		SET work_item_id = COALESCE((
			SELECT work_item_id FROM lifecycle_work_item
			WHERE substr(lifecycle_delegate_job.execution_key, 1, length(work_item_id) + 1) = work_item_id || ':'
			ORDER BY length(work_item_id) DESC LIMIT 1
		), '')
		WHERE work_item_id = ''`)
	_, _ = s.db.ExecContext(ctx, `CREATE INDEX IF NOT EXISTS idx_lifecycle_delegate_job_work_item ON lifecycle_delegate_job(work_item_id)`)
	// Older lifecycle mappings predate opaque participant capabilities. The C
	// resource plane backfills agent_jobs first; copy those capabilities into
	// the Go-owned durable mapping when both tables are present.
	_, _ = s.db.ExecContext(ctx, `UPDATE lifecycle_delegate_job
		SET participant_token = COALESCE((SELECT participant_token FROM agent_jobs WHERE agent_jobs.id = lifecycle_delegate_job.job_id), '')
		WHERE participant_token = ''`)
	return nil
}

func (s *Store) SaveWorkflowDelegateJob(ctx context.Context, key, workItemID string, id int, participant string) error {
	if key == "" || id <= 0 {
		return errors.New("delegate execution key and job id are required")
	}
	_, err := s.db.ExecContext(ctx, `INSERT INTO lifecycle_delegate_job(execution_key,job_id,work_item_id,participant_token) VALUES(?,?,?,?) ON CONFLICT(execution_key) DO UPDATE SET job_id=excluded.job_id,work_item_id=excluded.work_item_id,participant_token=excluded.participant_token,cancel_attempts=0,updated_at=datetime('now')`, key, id, workItemID, participant)
	return err
}

type DelegateJobMapping struct {
	ExecutionKey string
	JobID        int
}

const terminalCancellationBatchSize = 8

// TerminalDelegateJobs returns durable resource-plane jobs owned by workflow
// items that can no longer execute. Retaining the mapping until remote
// cancellation is acknowledged makes a crash between lifecycle commit and
// cancellation recoverable on the next scheduler fill.
func (s *Store) TerminalDelegateJobs(ctx context.Context) ([]DelegateJobMapping, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	rows, err := tx.QueryContext(ctx, `SELECT mapping.execution_key,mapping.job_id
FROM lifecycle_delegate_job mapping
JOIN lifecycle_work_item item ON item.work_item_id=mapping.work_item_id
JOIN agent_jobs job ON job.id=mapping.job_id
WHERE item.state IN ('accepted','rejected','stopped','abandoned')
  AND job.status IN ('pending','running')
ORDER BY mapping.cancel_attempts,mapping.job_id LIMIT ?`, terminalCancellationBatchSize)
	if err != nil {
		return nil, err
	}
	var mappings []DelegateJobMapping
	for rows.Next() {
		var mapping DelegateJobMapping
		if err := rows.Scan(&mapping.ExecutionKey, &mapping.JobID); err != nil {
			return nil, err
		}
		mappings = append(mappings, mapping)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for _, mapping := range mappings {
		if _, err := tx.ExecContext(ctx, `UPDATE lifecycle_delegate_job SET cancel_attempts=cancel_attempts+1,updated_at=datetime('now') WHERE execution_key=? AND job_id=?`, mapping.ExecutionKey, mapping.JobID); err != nil {
			return nil, err
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return mappings, nil
}

func (s *Store) hasWorkItemColumn(ctx context.Context, wanted string) (bool, error) {
	rows, err := s.db.QueryContext(ctx, `PRAGMA table_info(lifecycle_work_item)`)
	if err != nil {
		return false, err
	}
	defer rows.Close()
	for rows.Next() {
		var cid, notnull, pk int
		var name, kind string
		var defaultValue any
		if err := rows.Scan(&cid, &name, &kind, &notnull, &defaultValue, &pk); err != nil {
			return false, err
		}
		if name == wanted {
			return true, nil
		}
	}
	return false, rows.Err()
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
	ParentID        string
	SourcePath      string
	MaxCostUSD      float64
}

var ErrAdmissionFull = errors.New("trigger admission full")

func (s *Store) CreateWorkItem(ctx context.Context, in CreateWorkItem) error {
	return s.createWorkItem(ctx, in, 0)
}

// AdmitRoot atomically applies root-workflow deduplication, the live admission
// cap, and insertion. A zero cap is unlimited. Keeping all three decisions in
// one transaction prevents scanner and manual-fire races from exceeding policy.
func (s *Store) AdmitRoot(ctx context.Context, in CreateWorkItem, cap int) error {
	if in.ParentID != "" {
		return errors.New("root admission cannot create a child work item")
	}
	return s.createWorkItem(ctx, in, cap)
}

func (s *Store) createWorkItem(ctx context.Context, in CreateWorkItem, cap int) error {
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
	if cap > 0 {
		var active int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM lifecycle_work_item WHERE parent_id='' AND state='active'`).Scan(&active); err != nil {
			return fmt.Errorf("count admitted root workflows: %w", err)
		}
		if active >= cap {
			return fmt.Errorf("%w (%d/%d active root workflows)", ErrAdmissionFull, active, cap)
		}
	}
	var inserted sql.Result
	if in.ParentID == "" {
		inserted, err = tx.ExecContext(ctx, `
INSERT INTO lifecycle_work_item
  (work_item_id, repo, proposal_path, workflow_name, workflow_version,
   current_stage, mode, submitter, parent_id, source_path, work_item_max_cost_usd)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, in.ID, in.Repo, in.ProposalPath, in.WorkflowName,
			in.WorkflowVersion, in.StartStage, in.Mode, in.Submitter, in.ParentID, in.SourcePath, in.MaxCostUSD)
	} else {
		// Parent eligibility and insertion are one SQLite statement. A concurrent
		// StopTree therefore either includes this child or wins first and makes the
		// SELECT yield no row; there is no check-then-insert orphan window.
		inserted, err = tx.ExecContext(ctx, `
INSERT INTO lifecycle_work_item
  (work_item_id, repo, proposal_path, workflow_name, workflow_version,
   current_stage, mode, submitter, parent_id, source_path, work_item_max_cost_usd)
SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
FROM lifecycle_work_item parent
WHERE parent.work_item_id=? AND parent.state='active'`, in.ID, in.Repo, in.ProposalPath,
			in.WorkflowName, in.WorkflowVersion, in.StartStage, in.Mode, in.Submitter, in.ParentID,
			in.SourcePath, in.MaxCostUSD, in.ParentID)
	}
	if err != nil {
		return fmt.Errorf("insert work item: %w", err)
	}
	if changed, err := inserted.RowsAffected(); err != nil || changed != 1 {
		return errors.New("parent work item is not active")
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
	ReservedCostUSD   float64 `json:"reserved_cost_usd"`
	ReservationState  string  `json:"-"`
	MaxCostUSD        float64 `json:"work_item_max_cost_usd"`
	OverrideCount     int     `json:"override_count"`
	ParentID          string  `json:"parent_id,omitempty"`
	Worktree          string  `json:"worktree,omitempty"`
	SourcePath        string  `json:"-"`
	UpdatedAt         string  `json:"updated_at"`
}

func (s *Store) WorkItem(ctx context.Context, id string) (WorkItem, error) {
	var item WorkItem
	err := s.db.QueryRowContext(ctx, `
SELECT work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage,
       state, mode, pause_reason, content_hash, pr_ref, submitter, cum_cost_usd, reserved_cost_usd, reservation_state,
       work_item_max_cost_usd, override_count, parent_id, worktree, source_path, updated_at
FROM lifecycle_work_item WHERE work_item_id = ?`, id).Scan(
		&item.ID, &item.Repo, &item.ProposalPath, &item.WorkflowName, &item.WorkflowVersion,
		&item.Stage, &item.State, &item.Mode, &item.PauseReason, &item.ContentHash, &item.PRRef,
		&item.Submitter, &item.CumulativeCostUSD, &item.ReservedCostUSD, &item.ReservationState, &item.MaxCostUSD, &item.OverrideCount, &item.ParentID, &item.Worktree, &item.SourcePath,
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

func GitProposalIdentity(proposalHash, workflow, mode string) string {
	return fmt.Sprintf("git:%s:%s:%s", proposalHash, workflow, mode)
}

// WorkItemByGitProposal finds both the current blob-based trigger identity and
// the older commit-qualified identity. A proposal that has not changed must not
// be filed again merely because its watched branch advanced.
func (s *Store) WorkItemByGitProposal(ctx context.Context, repo, proposalHash, workflow, mode string) (WorkItem, error) {
	identity := GitProposalIdentity(proposalHash, workflow, mode)
	legacySuffix := fmt.Sprintf(":%s:%s:%s", proposalHash, workflow, mode)
	var id string
	if err := s.db.QueryRowContext(ctx, `
SELECT work_item_id FROM lifecycle_work_item
WHERE repo = ? AND parent_id = '' AND
      (proposal_path = ? OR
       (substr(proposal_path, 1, 4) = 'git:' AND
        substr(proposal_path, -length(?)) = ?))
ORDER BY id LIMIT 1`, repo, identity, legacySuffix, legacySuffix).Scan(&id); err != nil {
		return WorkItem{}, fmt.Errorf("find work item by git proposal: %w", err)
	}
	return s.WorkItem(ctx, id)
}

func (s *Store) WorkItems(ctx context.Context) ([]WorkItem, error) {
	rows, err := s.db.QueryContext(ctx, `
SELECT work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage,
       state, mode, pause_reason, content_hash, pr_ref, submitter, cum_cost_usd, reserved_cost_usd,
       work_item_max_cost_usd, override_count, parent_id, worktree, source_path, updated_at
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
			&item.ContentHash, &item.PRRef, &item.Submitter, &item.CumulativeCostUSD, &item.ReservedCostUSD,
			&item.MaxCostUSD, &item.OverrideCount, &item.ParentID, &item.Worktree, &item.SourcePath, &item.UpdatedAt); err != nil {
			return nil, fmt.Errorf("scan work item: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate work items: %w", err)
	}
	return items, nil
}

func (s *Store) SetWorktree(ctx context.Context, workItemID, worktree string) error {
	result, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item SET worktree=?, updated_at=datetime('now') WHERE work_item_id=?`, worktree, workItemID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err != nil || changed != 1 {
		return errors.New("work item not found")
	}
	return nil
}

func (s *Store) SetPRRef(ctx context.Context, workItemID, prRef string) error {
	result, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item SET pr_ref=?, updated_at=datetime('now') WHERE work_item_id=?`, prRef, workItemID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err != nil || changed != 1 {
		return errors.New("work item not found")
	}
	return nil
}

func (s *Store) Children(ctx context.Context, parentID string) ([]WorkItem, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT work_item_id FROM lifecycle_work_item WHERE parent_id=? ORDER BY id`, parentID)
	if err != nil {
		return nil, err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			_ = rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		_ = rows.Close()
		return nil, err
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	out := make([]WorkItem, 0, len(ids))
	for _, id := range ids {
		item, err := s.WorkItem(ctx, id)
		if err != nil {
			return nil, err
		}
		out = append(out, item)
	}
	return out, nil
}

// ActiveRootCount is the admission pressure that matters: every admitted,
// nonterminal top-level run. Transient parks remain active because the
// scheduler owns their automatic retry; excluding them would turn a temporary
// runner outage into unbounded queue growth.
func (s *Store) ActiveRootCount(ctx context.Context) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM lifecycle_work_item
WHERE parent_id='' AND state='active'`).Scan(&count)
	if err != nil {
		return 0, fmt.Errorf("count active root workflows: %w", err)
	}
	return count, nil
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

func (s *Store) ExecutedTurnCount(ctx context.Context, workItemID string) (int, error) {
	var count int
	// A turn is exactly an advance or loop execution event. Transient parks,
	// operator resumes, gates, and retry bookkeeping remain in the audit log but
	// must not consume execution budget; otherwise an unavailable dependency can
	// exhaust max_turns without the workflow running and every resume immediately
	// parks again.
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM lifecycle_event WHERE work_item_id=? AND kind IN ('advance','loop')`, workItemID).Scan(&count)
	return count, err
}

func (s *Store) WorkflowBudget(ctx context.Context, workItemID string) (rootID string, spent, max float64, err error) {
	item, err := s.WorkItem(ctx, workItemID)
	if err != nil {
		return "", 0, 0, err
	}
	for item.ParentID != "" {
		item, err = s.WorkItem(ctx, item.ParentID)
		if err != nil {
			return "", 0, 0, err
		}
	}
	rootID, max = item.ID, item.MaxCostUSD
	err = s.db.QueryRowContext(ctx, `WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) SELECT COALESCE(SUM(cum_cost_usd),0) FROM lifecycle_work_item WHERE work_item_id IN tree`, rootID).Scan(&spent)
	return
}

type BudgetReservation struct {
	RootID     string
	Amount     float64
	MaxUSD     float64
	Allowed    bool
	Busy       bool
	ReplayOnly bool
}

func isSQLiteContention(err error) bool {
	if err == nil {
		return false
	}
	message := strings.ToLower(err.Error())
	return strings.Contains(message, "database is locked") || strings.Contains(message, "database is busy") || strings.Contains(message, "sqlite_busy")
}

func workflowBudgetRoot(ctx context.Context, tx *sql.Tx, workItemID string) (string, float64, error) {
	var rootID string
	var maxUSD float64
	err := tx.QueryRowContext(ctx, `WITH RECURSIVE ancestors(id,parent_id,max_usd) AS (
  SELECT work_item_id,parent_id,work_item_max_cost_usd FROM lifecycle_work_item WHERE work_item_id=?
  UNION ALL
  SELECT parent.work_item_id,parent.parent_id,parent.work_item_max_cost_usd
  FROM lifecycle_work_item parent JOIN ancestors child ON child.parent_id=parent.work_item_id
)
SELECT id,max_usd FROM ancestors WHERE parent_id='' LIMIT 1`, workItemID).Scan(&rootID, &maxUSD)
	return rootID, maxUSD, err
}

// ReserveWorkflowBudget atomically gives one runnable item a fair share of the
// uncommitted root budget. Reservations are durable so process restart cannot
// forget in-flight spend. An uncapped tree needs no reservation.
func (s *Store) ReserveWorkflowBudget(ctx context.Context, workItemID, owner string) (BudgetReservation, error) {
	if owner == "" {
		return BudgetReservation{}, errors.New("workflow budget reservation owner is required")
	}
	var lastErr error
	for attempt := 0; attempt < 6; attempt++ {
		out, retry, err := s.reserveWorkflowBudgetOnce(ctx, workItemID, owner)
		if !retry {
			return out, err
		}
		lastErr = err
		select {
		case <-ctx.Done():
			return BudgetReservation{}, ctx.Err()
		case <-time.After(time.Duration(1<<attempt) * time.Millisecond):
		}
	}
	return BudgetReservation{}, fmt.Errorf("reserve workflow budget after contention: %w", lastErr)
}

func (s *Store) reserveWorkflowBudgetOnce(ctx context.Context, workItemID, owner string) (BudgetReservation, bool, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return BudgetReservation{}, isSQLiteContention(err), err
	}
	defer tx.Rollback()
	rootID, maxUSD, err := workflowBudgetRoot(ctx, tx, workItemID)
	if err != nil {
		return BudgetReservation{}, false, fmt.Errorf("resolve workflow budget root: %w", err)
	}
	out := BudgetReservation{RootID: rootID, MaxUSD: maxUSD, Allowed: true}
	var current float64
	var reservationState, reservationOwner string
	if err := tx.QueryRowContext(ctx, `SELECT reserved_cost_usd,reservation_state,reservation_owner FROM lifecycle_work_item
WHERE work_item_id=? AND state='active' AND pause_reason=''`, workItemID).Scan(&current, &reservationState, &reservationOwner); err != nil {
		return BudgetReservation{}, false, fmt.Errorf("load workflow budget reservation: %w", err)
	}
	if reservationState != "" {
		out.Amount = current
		if reservationState == "actual" || reservationState == "unresolved" {
			// Replay is still exactly-once work: take ownership only from an owner
			// whose lease has lapsed, and hold a fresh lease while replaying so a
			// concurrent process cannot steal the reservation mid-reconciliation.
			out.ReplayOnly = true
			if reservationOwner != owner {
				var live int
				if err := tx.QueryRowContext(ctx, `SELECT reservation_lease_until > datetime('now')
FROM lifecycle_work_item WHERE work_item_id=?`, workItemID).Scan(&live); err != nil {
					return BudgetReservation{}, false, err
				}
				if live != 0 {
					out.Allowed, out.Busy, out.ReplayOnly = false, true, false
					return out, false, tx.Commit()
				}
			}
			result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item
SET reservation_owner=?,reservation_lease_until=datetime('now','+2 minutes')
WHERE work_item_id=? AND reservation_state=? AND reservation_owner=?
  AND (reservation_owner=? OR reservation_lease_until='' OR reservation_lease_until<=datetime('now'))`,
				owner, workItemID, reservationState, reservationOwner, owner)
			if err != nil {
				return BudgetReservation{}, isSQLiteContention(err), err
			}
			if changed, err := result.RowsAffected(); err != nil || changed != 1 {
				return BudgetReservation{}, true, errors.New("replay workflow budget ownership was acquired concurrently")
			}
			return out, false, tx.Commit()
		}
		if reservationState == "reserved" && reservationOwner != owner {
			var live int
			if err := tx.QueryRowContext(ctx, `SELECT reservation_lease_until > datetime('now')
FROM lifecycle_work_item WHERE work_item_id=?`, workItemID).Scan(&live); err != nil {
				return BudgetReservation{}, false, err
			}
			if live != 0 {
				out.Allowed, out.Busy = false, true
				return out, false, tx.Commit()
			}
			// An expired invocation may have crossed the provider boundary. Retain
			// its authorization as unresolved spend and permit only a durable replay.
			// The replay may replace this estimate with measured actual exactly once.
			out.ReplayOnly = true
			result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item
SET reservation_state='unresolved',reservation_owner=?,reservation_lease_until=datetime('now','+2 minutes')
WHERE work_item_id=? AND reservation_state='reserved' AND reservation_owner=? AND reservation_lease_until<=datetime('now')`, owner, workItemID, reservationOwner)
			if err != nil {
				return BudgetReservation{}, isSQLiteContention(err), err
			}
			if changed, err := result.RowsAffected(); err != nil || changed != 1 {
				return BudgetReservation{}, true, errors.New("expired workflow budget lease was acquired concurrently")
			}
			return out, false, tx.Commit()
		}
		if reservationState == "reserved" {
			_, _ = tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET reservation_lease_until=datetime('now','+2 minutes') WHERE work_item_id=? AND reservation_owner=?`, workItemID, owner)
		}
		return out, false, tx.Commit()
	}
	if maxUSD <= 0 {
		result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET reserved_cost_usd=0,reservation_state='reserved',reservation_owner=?,reservation_lease_until=datetime('now','+2 minutes')
WHERE work_item_id=? AND state='active' AND pause_reason='' AND reservation_state=''`, owner, workItemID)
		if err != nil {
			return BudgetReservation{}, isSQLiteContention(err), err
		}
		if changed, err := result.RowsAffected(); err != nil || changed != 1 {
			return BudgetReservation{}, true, errors.New("uncapped work item changed during invocation reservation")
		}
		return out, false, tx.Commit()
	}
	var spent, outstanding float64
	var runnable int
	if err := tx.QueryRowContext(ctx, `WITH RECURSIVE tree(id) AS (
  SELECT ? UNION ALL SELECT child.work_item_id FROM lifecycle_work_item child JOIN tree parent ON child.parent_id=parent.id
)
SELECT COALESCE(SUM(cum_cost_usd),0),COALESCE(SUM(reserved_cost_usd),0),
       SUM(CASE WHEN state='active' AND pause_reason='' AND reserved_cost_usd=0 THEN 1 ELSE 0 END)
FROM lifecycle_work_item WHERE work_item_id IN tree`, rootID).Scan(&spent, &outstanding, &runnable); err != nil {
		return BudgetReservation{}, isSQLiteContention(err), fmt.Errorf("load workflow budget availability: %w", err)
	}
	remaining := maxUSD - spent - outstanding
	if remaining <= 0 {
		out.Allowed = false
		return out, false, tx.Commit()
	}
	if runnable < 1 {
		runnable = 1
	}
	out.Amount = remaining / float64(runnable)
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET reserved_cost_usd=?,reservation_state='reserved',reservation_owner=?,reservation_lease_until=datetime('now','+2 minutes')
WHERE work_item_id=? AND state='active' AND pause_reason='' AND reserved_cost_usd=0`, out.Amount, owner, workItemID)
	if err != nil {
		return BudgetReservation{}, isSQLiteContention(err), fmt.Errorf("reserve workflow budget: %w", err)
	}
	if changed, err := result.RowsAffected(); err != nil || changed != 1 {
		return BudgetReservation{}, true, errors.New("work item changed during budget reservation")
	}
	if err := tx.Commit(); err != nil {
		return BudgetReservation{}, isSQLiteContention(err), err
	}
	return out, false, nil
}

// ReconcileWorkflowBudget replaces one estimate with actual cost while the
// engine's short per-root completion lock is held. Other runner calls remain
// concurrent; only accounting and the lifecycle transition are serialized.
func (s *Store) ReconcileWorkflowBudget(ctx context.Context, workItemID, owner string, actual float64) (bool, error) {
	// Reject non-finite cost at the durable boundary: a NaN/Inf written into
	// cum_cost_usd would make every later budget comparison fail and strand the
	// whole tree in budget_cap. Symmetric with the C-side isfinite guard.
	if actual < 0 || math.IsNaN(actual) || math.IsInf(actual, 0) {
		return false, errors.New("workflow cost must be finite and non-negative")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	rootID, maxUSD, err := workflowBudgetRoot(ctx, tx, workItemID)
	if err != nil {
		return false, err
	}
	var state, currentOwner string
	var currentAmount float64
	if err := tx.QueryRowContext(ctx, `SELECT reserved_cost_usd,reservation_state,reservation_owner
FROM lifecycle_work_item WHERE work_item_id=?`, workItemID).Scan(&currentAmount, &state, &currentOwner); err != nil {
		return false, err
	}
	if currentOwner != owner {
		return false, errors.New("workflow budget reservation is owned by another invocation")
	}
	if state != "actual" && state != "reserved" && state != "unresolved" {
		return false, errors.New("workflow budget reservation is not active")
	}
	if state == "actual" && currentAmount != actual {
		return false, fmt.Errorf("replayed workflow cost %.6f differs from retained actual %.6f", actual, currentAmount)
	}
	// Allowance is recomputed from durable state on every call, including an
	// 'actual' replay. A crash between reconciliation and the tree park must not
	// silently upgrade a denied over-budget decision into an approved one.
	allowed := true
	if maxUSD > 0 {
		var spent, otherReserved float64
		if err := tx.QueryRowContext(ctx, `WITH RECURSIVE tree(id) AS (
  SELECT ? UNION ALL SELECT child.work_item_id FROM lifecycle_work_item child JOIN tree parent ON child.parent_id=parent.id
)
SELECT COALESCE(SUM(cum_cost_usd),0),
       COALESCE(SUM(CASE WHEN work_item_id<>? THEN reserved_cost_usd ELSE 0 END),0)
FROM lifecycle_work_item WHERE work_item_id IN tree`, rootID, workItemID).Scan(&spent, &otherReserved); err != nil {
			return false, err
		}
		allowed = spent+otherReserved+actual <= maxUSD
	}
	if state == "actual" {
		return allowed, tx.Commit()
	}
	if _, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET reserved_cost_usd=?,reservation_state='actual'
WHERE work_item_id=? AND reservation_owner=? AND reservation_state IN ('reserved','unresolved')`, actual, workItemID, owner); err != nil {
		return false, err
	}
	return allowed, tx.Commit()
}

func (s *Store) ReleaseWorkflowBudget(ctx context.Context, workItemID, owner string) error {
	_, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item
SET reserved_cost_usd=0,reservation_state='',reservation_owner='',reservation_lease_until=''
WHERE work_item_id=? AND reservation_owner=? AND reservation_state='reserved'`, workItemID, owner)
	return err
}

func (s *Store) HeartbeatWorkflowBudget(ctx context.Context, workItemID, owner string) error {
	_, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item
SET reservation_lease_until=datetime('now','+2 minutes')
WHERE work_item_id=? AND reservation_owner=? AND reservation_state IN ('reserved','actual','unresolved')`, workItemID, owner)
	return err
}

// ParkRunnerFailure records the failure and updates its reservation in the same
// database transaction. Pre-dispatch failures release it, measured failures
// commit actual spend, and ambiguous post-dispatch failures retain an unresolved
// estimate for replay instead of pretending the estimate is actual cost.
func (s *Store) ParkRunnerFailure(ctx context.Context, workItemID, stage, owner, reason, detail string, dispatched, costKnown bool, actual float64) error {
	if workItemID == "" || stage == "" || owner == "" || reason == "" || actual < 0 ||
		math.IsNaN(actual) || math.IsInf(actual, 0) {
		return errors.New("complete runner failure coordinates and finite non-negative cost are required")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	state := ""
	amount := 0.0
	eventCost := 0.0
	if dispatched && costKnown {
		state, amount, eventCost = "", 0, actual
	} else if dispatched {
		state = "unresolved"
		if err := tx.QueryRowContext(ctx, `SELECT reserved_cost_usd FROM lifecycle_work_item WHERE work_item_id=? AND reservation_owner=? AND reservation_state IN ('reserved','unresolved')`, workItemID, owner).Scan(&amount); err != nil {
			return fmt.Errorf("retain unresolved workflow reservation: %w", err)
		}
		// CostUSD may contain measured spend from completed reroute attempts even
		// when the final dispatched attempt is ambiguous. Commit that known prefix
		// now and retain only the remaining authorization as unresolved.
		eventCost = actual
		if amount > actual {
			amount -= actual
		} else if amount > 0 {
			amount = 0
		}
	}
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item
SET pause_reason=?,paused_state=?,cum_cost_usd=cum_cost_usd+?,reserved_cost_usd=?,reservation_state=?,reservation_owner=CASE WHEN ?='' THEN '' ELSE ? END,reservation_lease_until='',updated_at=datetime('now')
WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason='' AND reservation_owner=?`,
		reason, stage, eventCost, amount, state, state, owner, workItemID, stage, owner)
	if err != nil {
		return err
	}
	if changed, err := result.RowsAffected(); err != nil || changed != 1 {
		return errors.New("runner failure reservation changed concurrently")
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) VALUES (?,?, 'pause','go-wfe',?,?)`, workItemID, stage, detail, eventCost); err != nil {
		return err
	}
	return tx.Commit()
}

// RunnerFailuresSinceProgress counts how many times this stage has parked on a
// runner failure without any intervening forward progress. 'redispatch' is
// deliberately NOT progress: a recovery that keeps re-dispatching work that
// keeps failing is exactly the no-progress loop this bounds.
// Capacity backpressure is excluded: the provider refused to start the work, so
// the delegate never got a chance to fail. Those waits are bounded separately by
// CapacityWaitsSinceProgress.
func (s *Store) RunnerFailuresSinceProgress(ctx context.Context, workItemID, stage string) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM lifecycle_event
WHERE work_item_id=? AND stage=? AND kind='pause'
  AND detail NOT LIKE 'capacity_backpressure:%'
  AND id > COALESCE((SELECT MAX(id) FROM lifecycle_event
                     WHERE work_item_id=? AND kind IN ('advance','loop','create')), 0)`,
		workItemID, stage, workItemID).Scan(&count)
	return count, err
}

// CapacityWaitsSinceProgress counts how many times this stage has parked waiting
// out provider capacity without any intervening forward progress.
func (s *Store) CapacityWaitsSinceProgress(ctx context.Context, workItemID, stage string) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM lifecycle_event
WHERE work_item_id=? AND stage=? AND kind='pause'
  AND detail LIKE 'capacity_backpressure:%'
  AND id > COALESCE((SELECT MAX(id) FROM lifecycle_event
                     WHERE work_item_id=? AND kind IN ('advance','loop','create')), 0)`,
		workItemID, stage, workItemID).Scan(&count)
	return count, err
}

// RecoverLostReplay resolves a replay-only invocation whose durable delegate
// result is gone (e.g. a restart killed the in-flight job). A reservation left
// 'unresolved' was an ambiguous, interrupted dispatch whose estimate never
// reached cum_cost_usd: the interruption is recoverable, so the reservation is
// released and the item is left runnable for a fresh dispatch (redispatch=true).
// A reservation left 'actual' means the cost was measured and reconciled but the
// result cannot be reproduced: re-running would double-bill known spend, so the
// measured cost is committed and the item is parked non-transiently for a human
// (redispatch=false). Only the current owner may recover.
func (s *Store) RecoverLostReplay(ctx context.Context, workItemID, stage, owner string) (redispatch bool, err error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	var state, currentOwner string
	var amount float64
	if err := tx.QueryRowContext(ctx, `SELECT reservation_state,reservation_owner,reserved_cost_usd
FROM lifecycle_work_item WHERE work_item_id=?`, workItemID).Scan(&state, &currentOwner, &amount); err != nil {
		return false, err
	}
	if currentOwner != owner {
		return false, errors.New("lost-replay reservation is owned by another invocation")
	}
	switch state {
	case "unresolved":
		// Recoverable interruption: drop the never-committed estimate and leave the
		// item runnable so the next scheduler pass re-dispatches fresh work.
		result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item
SET reserved_cost_usd=0,reservation_state='',reservation_owner='',reservation_lease_until='',updated_at=datetime('now')
WHERE work_item_id=? AND reservation_owner=? AND reservation_state='unresolved'`, workItemID, owner)
		if err != nil {
			return false, err
		}
		if changed, err := result.RowsAffected(); err != nil || changed != 1 {
			return false, errors.New("lost-replay reservation changed concurrently")
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) VALUES (?,?, 'redispatch','go-wfe','replay result lost; re-dispatching fresh',0)`, workItemID, stage); err != nil {
			return false, err
		}
		return true, tx.Commit()
	case "actual":
		// Known spend whose result is unreproducible: commit the measured cost and
		// park for a human rather than silently re-billing it.
		result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item
SET cum_cost_usd=cum_cost_usd+reserved_cost_usd,reserved_cost_usd=0,reservation_state='',reservation_owner='',
    reservation_lease_until='',pause_reason='replay_unrecoverable',paused_state=current_stage,updated_at=datetime('now')
WHERE work_item_id=? AND reservation_owner=? AND reservation_state='actual'`, workItemID, owner)
		if err != nil {
			return false, err
		}
		if changed, err := result.RowsAffected(); err != nil || changed != 1 {
			return false, errors.New("lost-replay reservation changed concurrently")
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) VALUES (?,?, 'pause','go-wfe','replay_unrecoverable: reconciled result lost, parked for human',?)`, workItemID, stage, amount); err != nil {
			return false, err
		}
		return false, tx.Commit()
	default:
		return false, fmt.Errorf("lost-replay reservation is not replayable (state=%q)", state)
	}
}

func (s *Store) ParkBudgetTree(ctx context.Context, rootID, completedItemID string, addedCost float64) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if addedCost > 0 && completedItemID != "" {
		if _, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item
SET cum_cost_usd=cum_cost_usd+?,reserved_cost_usd=0,reservation_state='',reservation_owner='',reservation_lease_until=''
WHERE work_item_id=?`, addedCost, completedItemID); err != nil {
			return err
		}
	}
	if _, err := tx.ExecContext(ctx, `WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id)
UPDATE lifecycle_work_item SET pause_reason='budget_cap',paused_state=current_stage,updated_at=datetime('now')
WHERE state='active' AND pause_reason='' AND work_item_id IN tree
  AND (reservation_state='' OR work_item_id=?)`, rootID, completedItemID); err != nil {
		return err
	}
	_, err = tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) SELECT work_item_id,current_stage,'pause','go-wfe','budget_cap',? FROM lifecycle_work_item WHERE work_item_id=?`, addedCost, rootID)
	if err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) ResumeWallCaps(ctx context.Context, maxResumes int) (int64, error) {
	if maxResumes <= 0 {
		return 0, nil
	}
	result, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item SET pause_reason='', paused_state='', override_count=override_count+1, updated_at=datetime('now') WHERE state='active' AND pause_reason='wall_cap' AND override_count<?`, maxResumes)
	if err != nil {
		return 0, err
	}
	return result.RowsAffected()
}

// AbandonExhaustedWallCaps is deliberately narrow: only a wall-cap park that
// has exhausted automatic resumes can become a true abandonment. Active
// refinement and documentation convergence update normally and are never
// classified as stale or delayed by this reaper.
func (s *Store) AbandonExhaustedWallCaps(ctx context.Context, maxResumes int, grace time.Duration) (int64, error) {
	if grace <= 0 || maxResumes < 0 {
		return 0, nil
	}
	cutoff := time.Now().UTC().Add(-grace).Format("2006-01-02 15:04:05")
	result, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item SET state='abandoned',pause_reason='',paused_state='',updated_at=datetime('now') WHERE state='active' AND pause_reason='wall_cap' AND override_count>=? AND updated_at<?`, maxResumes, cutoff)
	if err != nil {
		return 0, err
	}
	return result.RowsAffected()
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
    cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=0, reservation_state='',
    reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
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
	if kind != "loop" {
		// Clearing the stage we just LEFT is the point: it completed, so a later
		// revisit starts with a fresh budget. Clearing the stage we are entering
		// is not — it reset the counter that bounds a refinement loop. A gate that
		// loops to its author (gate --loop--> plan) is re-entered by the author's
		// own advance (plan --advance--> gate), and that advance was wiping the
		// gate's accumulated attempts, so its cap could never be reached and the
		// pair looped without bound. Observed: a plan gate at 63 loops against a
		// cap of 20, burning five hours before it happened to converge.
		if _, err := tx.ExecContext(ctx, `DELETE FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?`, workItemID, fromStage); err != nil {
			return fmt.Errorf("clear completed stage attempts: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit stage transition: %w", err)
	}
	return nil
}

func (s *Store) RecordRetry(ctx context.Context, workItemID, stage, toStage, detail string, maxAttempts int, costUSD float64) (bool, error) {
	if maxAttempts < 1 {
		return false, errors.New("retry limit must be positive")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_stage_attempt (work_item_id,stage,attempts) VALUES (?,?,1) ON CONFLICT(work_item_id,stage) DO UPDATE SET attempts=attempts+1`, workItemID, stage); err != nil {
		return false, err
	}
	var attempts int
	if err := tx.QueryRowContext(ctx, `SELECT attempts FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?`, workItemID, stage).Scan(&attempts); err != nil {
		return false, err
	}
	parked := attempts >= maxAttempts
	reason := ""
	pausedState := ""
	target := toStage
	if parked {
		reason = "retry_limit"
		pausedState = stage
		target = stage
	}
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET current_stage=?,pause_reason=?,paused_state=?,cum_cost_usd=cum_cost_usd+?,reserved_cost_usd=0,reservation_state='',reservation_owner='',reservation_lease_until='',updated_at=datetime('now') WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''`, target, reason, pausedState, costUSD, workItemID, stage)
	if err != nil {
		return false, err
	}
	if changed, _ := result.RowsAffected(); changed != 1 {
		return false, errors.New("work item changed concurrently or is not runnable")
	}
	kind := "loop"
	if parked {
		kind = "pause"
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,cost_usd) VALUES (?,?,?,'go-wfe',?,?)`, workItemID, stage, kind, detail, costUSD); err != nil {
		return false, err
	}
	return parked, tx.Commit()
}

// StageLoopCount is durable fanout-generation state. Failed fanout loops and
// prior successful passes both count, so downstream refinement cannot reuse
// terminal children when the regenerated packet content is identical.
func (s *Store) StageLoopCount(ctx context.Context, workItemID, stage string) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `SELECT count(*) FROM lifecycle_event WHERE work_item_id=? AND stage=? AND kind IN ('loop','advance')`, workItemID, stage).Scan(&count)
	return count, err
}

// StageAttemptCount reports the current consecutive retry count for a stage.
// Unlike StageLoopCount, this counter is cleared when the stage advances, so it
// distinguishes a newly reviewed repair from a verifier failure that has just
// looped back to the same implementation stage.
func (s *Store) StageAttemptCount(ctx context.Context, workItemID, stage string) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `SELECT COALESCE((SELECT attempts FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?), 0)`, workItemID, stage).Scan(&count)
	return count, err
}

// LatestStageRetryDetail returns the diagnostic that caused the current stage
// retry. A loop is preferred over later transient runner pauses so cancellation
// recovery does not hide the verifier failure the next delegate must repair.
// When an operator resumes a retry-limit park, the park diagnostic is retained
// while the numeric attempt budget starts fresh. A successful advance is the
// boundary that makes all earlier diagnostics stale.
func (s *Store) LatestStageRetryDetail(ctx context.Context, workItemID, stage string) (string, error) {
	var boundary, reset int64
	attempts, err := s.StageAttemptCount(ctx, workItemID, stage)
	if err != nil {
		return "", err
	}
	if err := s.db.QueryRowContext(ctx, `SELECT COALESCE(MAX(id),0) FROM lifecycle_event WHERE work_item_id=? AND stage=? AND kind='advance'`, workItemID, stage).Scan(&boundary); err != nil {
		return "", err
	}
	if err := s.db.QueryRowContext(ctx, `SELECT COALESCE(MAX(id),0) FROM lifecycle_event WHERE work_item_id=? AND stage=? AND kind='resume' AND detail='retry_limit' AND id>?`, workItemID, stage, boundary).Scan(&reset); err != nil {
		return "", err
	}
	if attempts == 0 && reset <= boundary {
		return "", nil
	}
	var detail string
	err = s.db.QueryRowContext(ctx, `SELECT detail FROM lifecycle_event WHERE work_item_id=? AND stage=? AND kind='loop' AND id>? ORDER BY id DESC LIMIT 1`, workItemID, stage, max(boundary, reset)).Scan(&detail)
	if errors.Is(err, sql.ErrNoRows) && reset > boundary {
		err = s.db.QueryRowContext(ctx, `SELECT detail FROM lifecycle_event WHERE work_item_id=? AND stage=? AND kind='pause' AND detail!='manual' AND id>? AND id<? ORDER BY id DESC LIMIT 1`, workItemID, stage, boundary, reset).Scan(&detail)
	}
	if errors.Is(err, sql.ErrNoRows) {
		err = s.db.QueryRowContext(ctx, `SELECT detail FROM lifecycle_event WHERE work_item_id=? AND stage=? AND kind='pause' AND detail!='manual' AND id>? ORDER BY id DESC LIMIT 1`, workItemID, stage, boundary).Scan(&detail)
	}
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	return detail, err
}

// Park records the stable pause reason as both item state and event detail.
// Call ParkWithDetail when an operator-safe diagnostic should accompany it.
func (s *Store) Park(ctx context.Context, workItemID, stage, reason string, costUSD float64) error {
	return s.ParkWithDetail(ctx, workItemID, stage, reason, reason, costUSD)
}

// ParkWithDetail keeps the stable, machine-readable pause reason on the work
// item while retaining the complete diagnostic cause in the append-only event.
func (s *Store) ParkWithDetail(ctx context.Context, workItemID, stage, reason, detail string, costUSD float64) error {
	if workItemID == "" || stage == "" || reason == "" {
		return errors.New("work item, stage, and pause reason are required")
	}
	if detail == "" {
		detail = reason
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin park transition: %w", err)
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET pause_reason=?, paused_state=?, cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=0,
    reservation_state='', reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
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
VALUES (?, ?, 'pause', 'go-wfe', ?, ?)`, workItemID, stage, detail, costUSD); err != nil {
		return fmt.Errorf("record park transition: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit park transition: %w", err)
	}
	return nil
}

func (s *Store) Resume(ctx context.Context, workItemID string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var stage, reason string
	if err := tx.QueryRowContext(ctx, `SELECT current_stage, pause_reason FROM lifecycle_work_item WHERE work_item_id=? AND state='active'`, workItemID).Scan(&stage, &reason); err != nil {
		return fmt.Errorf("load resumable workflow: %w", err)
	}
	if reason == "" {
		return errors.New("workflow is not paused")
	}
	// delegate_failed parks explicitly for a human, so a human must be able to
	// release it once the underlying delegate problem is addressed.
	operatorReasons := map[string]bool{"manual": true, "wall_cap": true, "turn_cap": true, "retry_limit": true, "convergence_limit": true, "convergence_no_progress": true, "budget_cap": true, "fanout_limit": true, "workflow_definition_invalid": true, "workflow_block_unavailable": true, "delegate_failed": true, "replay_unrecoverable": true,
		// A conflicting parent integration is deliberately aborted and parked with
		// a clean worktree. An operator must resolve/commit the integration before
		// releasing the item; no scheduler-owned transition can do that work.
		"base_integration_conflict": true,
		// The roundtable judged the request itself unimplementable. A human amends
		// the request and resumes; nothing the engine can do releases it.
		"request_unimplementable": true}
	if !operatorReasons[reason] {
		return fmt.Errorf("pause reason %q is lifecycle-owned and cannot be resumed manually", reason)
	}
	if _, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET pause_reason='', paused_state='', updated_at=datetime('now') WHERE work_item_id=? AND state='active'`, workItemID); err != nil {
		return err
	}
	if reason == "retry_limit" {
		// A human resume is an explicit request for another bounded repair cycle.
		// Keeping the exhausted value made the very next failed repair park again,
		// effectively reducing recovery to one attempt per manual resume.
		if _, err := tx.ExecContext(ctx, `DELETE FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?`, workItemID, stage); err != nil {
			return err
		}
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail) VALUES (?, ?, 'resume', 'operator', ?)`, workItemID, stage, reason); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) ResolveGate(ctx context.Context, workItemID, fromStage, toStage, decision, contentHash string) error {
	if decision != "approve" && decision != "reject" {
		return errors.New("invalid gate decision")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET current_stage=?,pause_reason='',paused_state='',content_hash=?,updated_at=datetime('now') WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason='human_gate'`, toStage, contentHash, workItemID, fromStage)
	if err != nil {
		return err
	}
	if changed, _ := result.RowsAffected(); changed != 1 {
		return errors.New("workflow is not waiting at this human gate")
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,content_hash) VALUES (?,?,'gate','operator',?,?)`, workItemID, fromStage, decision, contentHash); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) RejectGate(ctx context.Context, workItemID, stage, contentHash string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET state='rejected',pause_reason='',paused_state='',content_hash=?,updated_at=datetime('now') WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason='human_gate'`, contentHash, workItemID, stage)
	if err != nil {
		return err
	}
	if changed, _ := result.RowsAffected(); changed != 1 {
		return errors.New("workflow is not waiting at this human gate")
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,content_hash) VALUES (?,?,'terminal','operator','human rejection',?)`, workItemID, stage, contentHash); err != nil {
		return err
	}
	return tx.Commit()
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

// StopTree atomically terminalizes a work item and every active descendant and
// returns the exact committed IDs so the scheduler can cancel the same set
// without a fallible post-commit discovery query.
func (s *Store) StopTree(ctx context.Context, workItemID string) ([]string, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	// Acquire SQLite's write reservation before reading the tree. Child creation
	// uses an INSERT...SELECT against the parent's active state, so either it
	// commits first and is included below, or it waits and then observes stopped.
	locked, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET state=state WHERE work_item_id=? AND state='active'`, workItemID)
	if err != nil {
		return nil, fmt.Errorf("lock workflow tree root: %w", err)
	}
	if changed, err := locked.RowsAffected(); err != nil || changed != 1 {
		return nil, errors.New("work item is not active")
	}
	type terminalItem struct {
		id, stage, contentHash string
	}
	rows, err := tx.QueryContext(ctx, `
WITH RECURSIVE tree(id) AS (
  SELECT ?
  UNION ALL
  SELECT child.work_item_id
  FROM lifecycle_work_item child JOIN tree parent ON child.parent_id=parent.id
)
SELECT item.work_item_id, item.current_stage, item.content_hash
FROM lifecycle_work_item item JOIN tree ON tree.id=item.work_item_id
WHERE item.state='active'`, workItemID)
	if err != nil {
		return nil, fmt.Errorf("list active workflow tree: %w", err)
	}
	var items []terminalItem
	for rows.Next() {
		var item terminalItem
		if err := rows.Scan(&item.id, &item.stage, &item.contentHash); err != nil {
			rows.Close()
			return nil, fmt.Errorf("scan active workflow tree: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate active workflow tree: %w", err)
	}
	for _, item := range items {
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,content_hash)
VALUES (?,?,'terminal','go-wfe','operator_stop',?)`, item.id, item.stage, item.contentHash); err != nil {
			return nil, fmt.Errorf("record stopped workflow tree: %w", err)
		}
	}
	result, err := tx.ExecContext(ctx, `
WITH RECURSIVE tree(id) AS (
  SELECT ?
  UNION ALL
  SELECT child.work_item_id
  FROM lifecycle_work_item child JOIN tree parent ON child.parent_id=parent.id
)
UPDATE lifecycle_work_item
SET state='stopped', pause_reason='', paused_state='', reserved_cost_usd=0,
    reservation_state='', reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
WHERE state='active' AND work_item_id IN (SELECT id FROM tree)`, workItemID)
	if err != nil {
		return nil, fmt.Errorf("stop workflow tree: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil || changed != int64(len(items)) {
		return nil, errors.New("workflow tree changed concurrently")
	}
	if _, err := tx.ExecContext(ctx, `WITH RECURSIVE tree(id) AS (
  SELECT ? UNION ALL SELECT child.work_item_id FROM lifecycle_work_item child JOIN tree parent ON child.parent_id=parent.id
) DELETE FROM wfe_frozen_create WHERE work_item_id IN tree`, workItemID); err != nil {
		return nil, fmt.Errorf("release stopped frozen-create claims: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	ids := make([]string, len(items))
	for i, item := range items {
		ids[i] = item.id
	}
	sort.Strings(ids)
	return ids, nil
}

// ReconcileOrphanedDescendants terminalizes active descendants whose ancestor
// is already terminal. Older servers could stop a parent without stopping its
// children, allowing those children to consume scheduler slots indefinitely.
func (s *Store) ReconcileOrphanedDescendants(ctx context.Context) ([]string, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	type orphan struct {
		id, stage, contentHash string
	}
	rows, err := tx.QueryContext(ctx, `
WITH RECURSIVE orphan(id) AS (
  SELECT child.work_item_id
  FROM lifecycle_work_item child
  JOIN lifecycle_work_item parent ON parent.work_item_id=child.parent_id
  WHERE child.state='active' AND parent.state IN ('accepted','rejected','stopped','abandoned')
  UNION
  SELECT child.work_item_id
  FROM lifecycle_work_item child JOIN orphan parent ON child.parent_id=parent.id
  WHERE child.state='active'
)
SELECT item.work_item_id, item.current_stage, item.content_hash
FROM lifecycle_work_item item JOIN orphan ON orphan.id=item.work_item_id`)
	if err != nil {
		return nil, fmt.Errorf("list orphaned workflow descendants: %w", err)
	}
	var items []orphan
	for rows.Next() {
		var item orphan
		if err := rows.Scan(&item.id, &item.stage, &item.contentHash); err != nil {
			rows.Close()
			return nil, fmt.Errorf("scan orphaned workflow descendant: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate orphaned workflow descendants: %w", err)
	}
	if len(items) == 0 {
		return nil, tx.Commit()
	}
	for _, item := range items {
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id,stage,kind,actor,detail,content_hash)
VALUES (?,?,'terminal','go-wfe','ancestor_terminal',?)`, item.id, item.stage, item.contentHash); err != nil {
			return nil, fmt.Errorf("record orphaned workflow descendant: %w", err)
		}
	}
	result, err := tx.ExecContext(ctx, `
WITH RECURSIVE orphan(id) AS (
  SELECT child.work_item_id
  FROM lifecycle_work_item child
  JOIN lifecycle_work_item parent ON parent.work_item_id=child.parent_id
  WHERE child.state='active' AND parent.state IN ('accepted','rejected','stopped','abandoned')
  UNION
  SELECT child.work_item_id
  FROM lifecycle_work_item child JOIN orphan parent ON child.parent_id=parent.id
  WHERE child.state='active'
)
UPDATE lifecycle_work_item
SET state='stopped', pause_reason='', paused_state='', reserved_cost_usd=0,
    reservation_state='', reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
WHERE state='active' AND work_item_id IN (SELECT id FROM orphan)`)
	if err != nil {
		return nil, fmt.Errorf("stop orphaned workflow descendants: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil || changed != int64(len(items)) {
		return nil, errors.New("orphaned workflow descendants changed concurrently")
	}
	for _, item := range items {
		if _, err := tx.ExecContext(ctx, `DELETE FROM wfe_frozen_create WHERE work_item_id=?`, item.id); err != nil {
			return nil, fmt.Errorf("release orphan frozen-create claim: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	ids := make([]string, len(items))
	for i, item := range items {
		ids[i] = item.id
	}
	sort.Strings(ids)
	return ids, nil
}

func (s *Store) Delete(ctx context.Context, workItemID string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var active int
	if err := tx.QueryRowContext(ctx, `WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) SELECT count(*) FROM lifecycle_work_item WHERE work_item_id IN tree AND state='active'`, workItemID).Scan(&active); err != nil {
		return err
	}
	if active > 0 {
		return errors.New("workflow tree contains active items that must be stopped before deletion")
	}
	for _, query := range []string{
		`WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) DELETE FROM wfe_frozen_create WHERE work_item_id IN tree OR parent_id IN tree`,
		`WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) DELETE FROM wfe_convergence WHERE work_item_id IN tree`,
		`WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) DELETE FROM lifecycle_stage_attempt WHERE work_item_id IN tree`,
		`WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) DELETE FROM lifecycle_event WHERE work_item_id IN tree`,
		`WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) DELETE FROM lifecycle_work_item WHERE work_item_id IN tree`,
	} {
		if _, err := tx.ExecContext(ctx, query, workItemID); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func (s *Store) DescendantIDs(ctx context.Context, workItemID string) ([]string, error) {
	rows, err := s.db.QueryContext(ctx, `WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) SELECT id FROM tree`, workItemID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
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

func (s *Store) ResumeReadyParents(ctx context.Context) (int64, error) {
	result, err := s.db.ExecContext(ctx, `UPDATE lifecycle_work_item AS parent
SET pause_reason='', paused_state='', updated_at=datetime('now')
WHERE parent.state='active' AND parent.pause_reason='slices_running'
  AND EXISTS (SELECT 1 FROM lifecycle_work_item child WHERE child.parent_id=parent.work_item_id)
  AND NOT EXISTS (SELECT 1 FROM lifecycle_work_item child WHERE child.parent_id=parent.work_item_id AND child.state='active')`)
	if err != nil {
		return 0, fmt.Errorf("resume parents with terminal children: %w", err)
	}
	return result.RowsAffected()
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
    cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=0, reservation_state='',
    reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
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
	if state != "accepted" {
		if _, err := tx.ExecContext(ctx, `DELETE FROM wfe_frozen_create WHERE work_item_id=?`, workItemID); err != nil {
			return fmt.Errorf("release terminal frozen-create claim: %w", err)
		}
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
// unresolved names the findings still blocking when the gate runs out of rounds.
// Without it the park records the bare reason "convergence_limit", which says a
// budget was spent but not what was never fixed -- leaving a human to reconstruct
// it from the last feedback artifact. A gate that burned every round on the same
// finding is evidence about the plan or the request; the park should carry it.
func (s *Store) RecordRequestedChanges(ctx context.Context, workItemID, gate, planStage,
	planHash, feedbackHash, unresolved string, maxIterations, maxIdentical int, costUSD float64) (ReviewOutcome, error) {
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
SET current_stage=?, pause_reason=?, paused_state=?, content_hash=?, cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=0, reservation_state='', reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
WHERE work_item_id=?`, planStage, out.PauseReason, gate, planHash, costUSD, workItemID); err != nil {
			return ReviewOutcome{}, fmt.Errorf("park non-converging work item: %w", err)
		}
		detail := out.PauseReason
		if strings.TrimSpace(unresolved) != "" {
			detail = fmt.Sprintf("%s after %d rounds; still unresolved: %s", out.PauseReason, attempts, unresolved)
		}
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
VALUES (?, ?, 'pause', 'go-wfe', ?, ?, ?)`, workItemID, gate, detail, planHash, costUSD); err != nil {
			return ReviewOutcome{}, fmt.Errorf("record convergence park: %w", err)
		}
	} else {
		if _, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET current_stage=?, pause_reason='', paused_state='', content_hash=?, cum_cost_usd=cum_cost_usd+?, reserved_cost_usd=0, reservation_state='', reservation_owner='', reservation_lease_until='', updated_at=datetime('now')
WHERE work_item_id=?`, planStage, planHash, costUSD, workItemID); err != nil {
			return ReviewOutcome{}, fmt.Errorf("route work item to plan refinement: %w", err)
		}
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
VALUES (?, ?, 'loop', 'go-wfe', 'requested_changes', ?, ?)`, workItemID, gate, planHash, costUSD); err != nil {
			return ReviewOutcome{}, fmt.Errorf("record review loop: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return ReviewOutcome{}, fmt.Errorf("commit review transition: %w", err)
	}
	return out, nil
}
