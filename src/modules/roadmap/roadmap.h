/* roadmap.h: spec-driven roadmap hierarchy + autonomous dispatch contract.
 *
 * A roadmap decomposes one goal into a dependency-aware tree of milestones,
 * slices, and leaf tasks. Durable `roadmap` / `plan_unit` artifacts live in
 * DB2 (see db2/roadmap.sql); single-host runtime dispatch state lives in DB1
 * (see db1/roadmap_runtime.sql). Leaf tasks are executed by delegating to the
 * existing delegate / coord-job substrate; the loop only decomposes, selects,
 * gates, and advances.
 *
 * See docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
 * and docs/proposals/done/architecture-charter.md (Plan/Search + Synthesize).
 *
 * Phase-1 scaffold: only the data model + roadmap new/show/projection contract
 * is defined here. The dispatch loop (roadmap_auto_*) lands in a later phase. */
#ifndef AIMEE_ROADMAP_H
#define AIMEE_ROADMAP_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Decomposition level of a plan_unit within a roadmap. */
   typedef enum
   {
      ROADMAP_LEVEL_MILESTONE = 0,
      ROADMAP_LEVEL_SLICE = 1,
      ROADMAP_LEVEL_TASK = 2
   } roadmap_level_t;

   /* Lifecycle of a plan_unit as the loop advances. Distinct from the charter
    * artifact state (proposed/committed/...): this is the in-roadmap status
    * carried in the unit payload and the DB1 dispatch row. */
   typedef enum
   {
      ROADMAP_UNIT_PENDING = 0,
      ROADMAP_UNIT_ACTIVE = 1,
      ROADMAP_UNIT_DONE = 2,
      ROADMAP_UNIT_BLOCKED = 3,
      ROADMAP_UNIT_NEEDS_REVIEW = 4
   } roadmap_unit_state_t;

   /* Per-unit tool-policy mode, mapped onto existing guardrails (plan mode,
    * capability scope, worktree path enforcement) at dispatch time. */
   typedef enum
   {
      ROADMAP_POLICY_PLANNING = 0, /* read broad; write only the .aimee/ tree */
      ROADMAP_POLICY_DOCS = 1,     /* + write the docs/ tree, top-level READMEs */
      ROADMAP_POLICY_EXECUTION = 2 /* write owned_files; full delegation */
   } roadmap_tool_policy_t;

   /* plan_unit payload contract (carried in artifacts.payload as JSON):
    *   level            milestone | slice | task
    *   parent_id        artifact id of the containing unit ("" for milestones)
    *   title, intent    human-readable
    *   depends_on       [unit_id, ...] sibling DAG within the parent
    *   owned_files      [path, ...] (tasks only; the delegate conflict set)
    *   read_context     [path, ...]
    *   acceptance_criteria [string, ...]
    *   verification_commands [string, ...]
    *   tool_policy_mode planning | docs | execution
    *   plan_candidate_ref artifact id of the deliberate-planner step plan
    *   summary_ref      artifact id of the unit SUMMARY when done
    *   state            pending | active | done | blocked | needs_review
    *
    * roadmap payload contract:
    *   goal, requirements_ref, planning_depth, token_profile, unit_ids[] */

   /* Decomposition provider hook. The dispatch/server layer registers a
    * function that runs the reason/draft delegate and returns the decomposition
    * JSON document (see roadmap_create_from_decomposition for its shape) in
    * *out_json (heap-allocated; caller frees). Returns 0 on success, -1 on
    * error. Kept as an injectable hook so the deterministic create/validate path
    * is unit-testable without a live model, and so roadmap_new stays free of any
    * direct delegate/server coupling. */
   typedef int (*roadmap_decompose_fn)(const char *goal, const char *planning_depth,
                                       const char *token_profile, char **out_json, void *ud);
   void roadmap_set_decomposer(roadmap_decompose_fn fn, void *ud);

   /* Phase 1 — create a roadmap from a goal. Decomposition is delegated to a
    * reason/draft delegate via the registered decomposer; the proposed roadmap +
    * plan_unit artifacts pass structural DAG validation before commit (no LLM
    * writes state directly). On success writes the committed roadmap id into
    * out_id (caller-provided buffer). Returns 0 on success, -1 on error
    * (including when no decomposer is registered). */
   int roadmap_new(const char *goal, const char *planning_depth, const char *token_profile,
                   char *out_id, size_t out_len);

   /* Phase 1 — deterministic core: validate a decomposition document and, if it
    * is structurally sound, write the roadmap + plan_unit artifacts (proposed),
    * then commit them (proposed -> committed). On success writes the committed
    * roadmap artifact id into out_id. Returns 0 on success, -1 on error.
    *
    * decomposition_json shape (units reference each other by local_id):
    *   {"goal": "...",
    *    "planning_depth": "standard",       // optional; default "standard"
    *    "token_profile": "balanced",        // optional; default "balanced"
    *    "units": [
    *      {"local_id":"m1","level":"milestone","parent":"","title":"...",
    *       "intent":"...","depends_on":[],"acceptance_criteria":[...],
    *       "verification_commands":[...],"tool_policy_mode":"planning"},
    *      {"local_id":"s1","level":"slice","parent":"m1", ...},
    *      {"local_id":"t1","level":"task","parent":"s1",
    *       "owned_files":["src/x.c"],"acceptance_criteria":["..."], ...}
    *    ]}
    * parent/depends_on reference sibling local_ids; the function resolves them to
    * the generated artifact ids in the committed payloads. */
   int roadmap_create_from_decomposition(const char *decomposition_json, char *out_id,
                                         size_t out_len);

   /* Phase 1 — validate a decomposition document structurally without writing
    * anything. Checks: non-empty goal; valid levels (milestone|slice|task) with
    * correct nesting (milestone > slice > task); unique local_ids; parent refs
    * resolve; depends_on refs are same-parent siblings and acyclic (DAG); leaf
    * tasks carry non-empty owned_files and acceptance_criteria; valid
    * tool_policy_mode. On failure, writes a human-readable reason into err (if
    * non-NULL). Returns 0 if valid, -1 otherwise. */
   int roadmap_validate_decomposition(const char *decomposition_json, char *err, size_t err_len);

   /* Phase 1 — render the roadmap tree + state. json_output != 0 emits JSON;
    * otherwise prints the human projection. Returns 0 on success, -1 on error. */
   int roadmap_show(const char *roadmap_id, int json_output);

   /* Phase 1 — build the roadmap tree as a JSON document and return it
    * heap-allocated in *out (caller frees). Shape:
    *   {"roadmap": <roadmap payload>, "units": [<plan_unit payload>, ...]}
    * This is the data path the `roadmap.show` kb RPC returns to the CLI, so the
    * tree can be rendered client-side without the kb sidecar writing to stdout.
    * Returns 0 on success, -1 on error. */
   int roadmap_show_json(const char *roadmap_id, char **out);

   /* Phase 1 — list committed roadmaps as a JSON document in *out (caller
    * frees). Shape:
    *   {"roadmaps": [{"id":"...","goal":"...","status_rollup":{...}}, ...]}
    * Backs `aimee roadmap list` / `status`. Returns 0 / -1. */
   int roadmap_list_json(char **out);

   /* Phase 1 — refresh .aimee/roadmap/ markdown projections (STATE.md,
    * ROADMAP.md, per-unit PLAN/SUMMARY) from committed artifacts. Projections
    * are read-only views; the artifacts are canonical. Returns 0 / -1. */
   int roadmap_projections_write(const char *roadmap_id);

   /* Phase 1 — regenerate projections from artifacts (identity rebuild used by
    * `aimee roadmap rebuild` and the projection round-trip test). Returns 0 / -1. */
   int roadmap_projections_rebuild(const char *roadmap_id);

   /* Phase 3 — generate a self-contained HTML milestone progress report for
    * roadmap_id and write it to output_path (e.g.
    * ".aimee/roadmap/reports/<id>.html"). When output_path is NULL the default
    * path is used. Returns 0 on success, -1 on error. */
   int roadmap_report_html(const char *roadmap_id, const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_ROADMAP_H */
