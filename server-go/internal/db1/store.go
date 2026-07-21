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
CREATE TABLE IF NOT EXISTS lifecycle_delegate_job (
  execution_key TEXT PRIMARY KEY,
  job_id INTEGER NOT NULL,
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
	return nil
}

func (s *Store) DelegateJob(ctx context.Context, key string) (int, error) {
	var id int
	err := s.db.QueryRowContext(ctx, `SELECT job_id FROM lifecycle_delegate_job WHERE execution_key=?`, key).Scan(&id)
	return id, err
}

func (s *Store) SaveDelegateJob(ctx context.Context, key string, id int) error {
	if key == "" || id <= 0 {
		return errors.New("delegate execution key and job id are required")
	}
	_, err := s.db.ExecContext(ctx, `INSERT INTO lifecycle_delegate_job(execution_key,job_id) VALUES(?,?) ON CONFLICT(execution_key) DO UPDATE SET job_id=excluded.job_id,updated_at=datetime('now')`, key, id)
	return err
}

func (s *Store) ForgetDelegateJob(ctx context.Context, key string) error {
	_, err := s.db.ExecContext(ctx, `DELETE FROM lifecycle_delegate_job WHERE execution_key=?`, key)
	return err
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
	_, err = tx.ExecContext(ctx, `
INSERT INTO lifecycle_work_item
  (work_item_id, repo, proposal_path, workflow_name, workflow_version,
   current_stage, mode, submitter, parent_id, source_path, work_item_max_cost_usd)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, in.ID, in.Repo, in.ProposalPath, in.WorkflowName,
		in.WorkflowVersion, in.StartStage, in.Mode, in.Submitter, in.ParentID, in.SourcePath, in.MaxCostUSD)
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
	ParentID          string  `json:"parent_id,omitempty"`
	Worktree          string  `json:"worktree,omitempty"`
	SourcePath        string  `json:"-"`
	UpdatedAt         string  `json:"updated_at"`
}

func (s *Store) WorkItem(ctx context.Context, id string) (WorkItem, error) {
	var item WorkItem
	err := s.db.QueryRowContext(ctx, `
SELECT work_item_id, repo, proposal_path, workflow_name, workflow_version, current_stage,
       state, mode, pause_reason, content_hash, pr_ref, submitter, cum_cost_usd,
       work_item_max_cost_usd, override_count, parent_id, worktree, source_path, updated_at
FROM lifecycle_work_item WHERE work_item_id = ?`, id).Scan(
		&item.ID, &item.Repo, &item.ProposalPath, &item.WorkflowName, &item.WorkflowVersion,
		&item.Stage, &item.State, &item.Mode, &item.PauseReason, &item.ContentHash, &item.PRRef,
		&item.Submitter, &item.CumulativeCostUSD, &item.MaxCostUSD, &item.OverrideCount, &item.ParentID, &item.Worktree, &item.SourcePath,
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
			&item.ContentHash, &item.PRRef, &item.Submitter, &item.CumulativeCostUSD,
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

func (s *Store) ParkBudgetTree(ctx context.Context, rootID string, addedCost float64) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, `WITH RECURSIVE tree(id) AS (SELECT ? UNION ALL SELECT w.work_item_id FROM lifecycle_work_item w JOIN tree t ON w.parent_id=t.id) UPDATE lifecycle_work_item SET pause_reason='budget_cap',paused_state=current_stage,updated_at=datetime('now') WHERE state='active' AND pause_reason='' AND work_item_id IN tree`, rootID); err != nil {
		return err
	}
	if addedCost > 0 {
		if _, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET cum_cost_usd=cum_cost_usd+? WHERE work_item_id=?`, addedCost, rootID); err != nil {
			return err
		}
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
	if kind != "loop" {
		if _, err := tx.ExecContext(ctx, `DELETE FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?`, workItemID, fromStage); err != nil {
			return fmt.Errorf("clear completed stage attempts: %w", err)
		}
		if fromStage != toStage {
			if _, err := tx.ExecContext(ctx, `DELETE FROM lifecycle_stage_attempt WHERE work_item_id=? AND stage=?`, workItemID, toStage); err != nil {
				return err
			}
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
	result, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET current_stage=?,pause_reason=?,paused_state=?,cum_cost_usd=cum_cost_usd+?,updated_at=datetime('now') WHERE work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''`, target, reason, pausedState, costUSD, workItemID, stage)
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
	operatorReasons := map[string]bool{"manual": true, "wall_cap": true, "turn_cap": true, "retry_limit": true, "convergence_limit": true, "convergence_no_progress": true, "budget_cap": true, "fanout_limit": true, "workflow_definition_invalid": true, "workflow_block_unavailable": true}
	if !operatorReasons[reason] {
		return fmt.Errorf("pause reason %q is lifecycle-owned and cannot be resumed manually", reason)
	}
	if _, err := tx.ExecContext(ctx, `UPDATE lifecycle_work_item SET pause_reason='', paused_state='', updated_at=datetime('now') WHERE work_item_id=? AND state='active'`, workItemID); err != nil {
		return err
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
	item, err := s.WorkItem(ctx, workItemID)
	if err != nil {
		return err
	}
	return s.Finish(ctx, workItemID, item.Stage, "stopped", "operator_stop", item.ContentHash, 0)
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
	planHash, feedbackHash string, maxIterations, maxIdentical int, costUSD float64) (ReviewOutcome, error) {
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
SET current_stage=?, pause_reason=?, paused_state=?, content_hash=?, cum_cost_usd=cum_cost_usd+?, updated_at=datetime('now')
WHERE work_item_id=?`, planStage, out.PauseReason, gate, planHash, costUSD, workItemID); err != nil {
			return ReviewOutcome{}, fmt.Errorf("park non-converging work item: %w", err)
		}
		if _, err := tx.ExecContext(ctx, `
INSERT INTO lifecycle_event (work_item_id, stage, kind, actor, detail, content_hash, cost_usd)
VALUES (?, ?, 'pause', 'go-wfe', ?, ?, ?)`, workItemID, gate, out.PauseReason, planHash, costUSD); err != nil {
			return ReviewOutcome{}, fmt.Errorf("record convergence park: %w", err)
		}
	} else {
		if _, err := tx.ExecContext(ctx, `
UPDATE lifecycle_work_item
SET current_stage=?, pause_reason='', paused_state='', content_hash=?, cum_cost_usd=cum_cost_usd+?, updated_at=datetime('now')
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
