-- db2/roadmap.sql: spec-driven roadmap surfaces over the shared artifacts
-- table (DB2, postgres-native). No new base tables: `roadmap` and `plan_unit`
-- are charter artifact kinds carried in artifacts(kind, payload). See
-- docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
-- and docs/proposals/done/architecture-charter.md (promotion targets).
--
-- Phase-1 scaffold: this file is not yet embedded/applied by db2_init.c. It
-- defines the durable read surfaces the dispatch loop will query once wired.

-- Committed roadmaps (one active per goal; superseded on reassessment).
CREATE OR REPLACE VIEW roadmap_artifacts AS
  SELECT a.id,
         a.state,
         a.scope_kind,
         a.scope_id,
         (a.payload::jsonb)                      AS payload,
         (a.payload::jsonb) ->> 'goal'           AS goal,
         (a.payload::jsonb) ->> 'planning_depth' AS planning_depth,
         (a.payload::jsonb) ->> 'token_profile'  AS token_profile,
         a.created_at,
         a.committed_at,
         a.superseded_by
    FROM artifacts a
   WHERE a.kind = 'roadmap';

-- Milestone / slice / task nodes within a roadmap. The unit DAG
-- (parent_id, depends_on) lives in the payload, like plan_candidate subgoals.
CREATE OR REPLACE VIEW plan_unit_artifacts AS
  SELECT a.id,
         a.state,
         a.scope_kind,
         a.scope_id,
         (a.payload::jsonb)                  AS payload,
         (a.payload::jsonb) ->> 'level'      AS level,
         (a.payload::jsonb) ->> 'parent_id'  AS parent_id,
         (a.payload::jsonb) ->> 'title'      AS title,
         (a.payload::jsonb) ->> 'state'      AS unit_state,
         (a.payload::jsonb) -> 'depends_on'  AS depends_on,
         (a.payload::jsonb) -> 'owned_files' AS owned_files,
         a.created_at,
         a.committed_at
    FROM artifacts a
   WHERE a.kind = 'plan_unit';

-- Dispatch-loop hot paths: next-ready-unit selection filters by kind/state and
-- by the payload fields the selector reads (parent_id, unit state).
CREATE INDEX IF NOT EXISTS idx_artifacts_kind_state
  ON artifacts (kind, state);
CREATE INDEX IF NOT EXISTS idx_artifacts_plan_unit_parent
  ON artifacts (((payload::jsonb) ->> 'parent_id'))
  WHERE kind = 'plan_unit';
CREATE INDEX IF NOT EXISTS idx_artifacts_plan_unit_state
  ON artifacts (((payload::jsonb) ->> 'state'))
  WHERE kind = 'plan_unit';
