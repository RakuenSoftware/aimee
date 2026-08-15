-- db1/roadmap_runtime.sql: runtime dispatch state for the spec-driven roadmap
-- loop (DB1, sqlite). Durable roadmap/plan_unit artifacts live in DB2; this
-- file holds only the single-host live dispatch state (claims, leases,
-- heartbeats, retry counters) the loop reconciles against, mirroring the
-- coord_jobs / agent_jobs pattern in schema.sql.
-- See docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
--
-- Phase-2: tables are embedded in db1/schema.sql and applied by db1_init.c.

-- One row per active roadmap run (the loop's phase + safety ceilings).
CREATE TABLE IF NOT EXISTS roadmap_dispatch ( id INTEGER PRIMARY KEY AUTOINCREMENT, roadmap_id TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'running', phase TEXT NOT NULL DEFAULT 'plan', token_profile TEXT NOT NULL DEFAULT 'balanced', require_slice_discussion INTEGER NOT NULL DEFAULT 1, budget_ceiling_tokens INTEGER NOT NULL DEFAULT 0, exit_reason TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(roadmap_id));

-- Per-unit live dispatch state (claim / heartbeat / verify / retry). Leaf tasks
-- carry the coord_job_id of the delegate work packet executing them.
CREATE TABLE IF NOT EXISTS roadmap_unit_dispatch ( id INTEGER PRIMARY KEY AUTOINCREMENT, roadmap_id TEXT NOT NULL, unit_id TEXT NOT NULL, level TEXT NOT NULL DEFAULT 'task', state TEXT NOT NULL DEFAULT 'pending', tool_policy_mode TEXT NOT NULL DEFAULT 'execution', claimed_by TEXT NOT NULL DEFAULT '', claimed_at TEXT NOT NULL DEFAULT '', heartbeat_at TEXT NOT NULL DEFAULT '', verify_attempts INTEGER NOT NULL DEFAULT 0, dispatch_attempts INTEGER NOT NULL DEFAULT 0, worktree_path TEXT NOT NULL DEFAULT '', coord_job_id INTEGER NOT NULL DEFAULT 0, result TEXT NOT NULL DEFAULT '', error TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(roadmap_id, unit_id));

-- Milestone-level lease for parallel workers (one worker per milestone, safe
-- takeover after expiry), modeled on coord_jobs claim/lease semantics.
CREATE TABLE IF NOT EXISTS roadmap_milestone_lease ( milestone_id TEXT PRIMARY KEY, roadmap_id TEXT NOT NULL, lease_owner TEXT NOT NULL DEFAULT '', branch TEXT NOT NULL DEFAULT '', worktree_path TEXT NOT NULL DEFAULT '', heartbeat_at TEXT NOT NULL DEFAULT '', expires_at TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));

CREATE INDEX IF NOT EXISTS idx_roadmap_unit_dispatch_ready ON roadmap_unit_dispatch (roadmap_id, state);
