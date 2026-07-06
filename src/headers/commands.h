#ifndef DEC_COMMANDS_H
#define DEC_COMMANDS_H 1

#include <stdio.h>
#include "aimee.h"

/* Subcommand dispatch table entry. Handlers receive the app context only.
 * If a subcommand needs DB1, it calls db1_init(cfg.db1_path) directly; the
 * dispatch layer does not own a DB connection. */
typedef struct
{
   const char *name;
   const char *help;
   void (*handler)(app_ctx_t *ctx, int argc, char **argv);
} subcmd_t;

/* Look up and call a subcommand handler. Returns 0 on match, -1 if not found. */
int subcmd_dispatch(const subcmd_t *table, const char *name, app_ctx_t *ctx, int argc, char **argv);

/* Print subcommand usage for a parent command. */
void subcmd_usage(const char *parent, const subcmd_t *table);

/* cmd_core.c */
void ensure_mcp_json(const char *dir);
void ensure_client_integrations(void);
void ensure_codex_project_trusted(const char *codex_home, const char *project_root);
void ensure_codex_current_project_trusted(const char *codex_home);

/* client_integrations.c */
void ensure_claude_code_trust(const char *dir);
void cmd_init(app_ctx_t *ctx, int argc, char **argv);

/* Stack detection for aimee init */
typedef struct
{
   const char *name;
   const char *build_cmd;
   const char *test_cmd;
} stack_info_t;

#define MAX_DETECTED_STACKS 8

/* Probe dir for known technology stacks. Returns number of stacks found (0..MAX_DETECTED_STACKS).
 */
int detect_project_stacks(const char *dir, stack_info_t out[], int max_stacks);

/* Write .aimee-rules template for dir. Returns 0 on success, 1 if already exists, -1 on error. */
int write_aimee_rules_file(const char *dir, const stack_info_t *stacks, int count);

/* Write a story-bible .aimee-rules scaffold for novel mode (premise, voice,
 * characters, setting, timeline) instead of the engineering stack doc.
 * Returns 0 on success, 1 if already exists, -1 on error. */
int write_novel_rules_file(const char *dir);

/* Write a songbook .aimee-rules scaffold for songwriter mode (concept, voice,
 * structure, hook). Returns 0 on success, 1 if already exists, -1 on error. */
int write_songbook_rules_file(const char *dir);
void cmd_setup(app_ctx_t *ctx, int argc, char **argv);
void cmd_version(app_ctx_t *ctx, int argc, char **argv);
void cmd_mode(app_ctx_t *ctx, int argc, char **argv);
void cmd_primary(app_ctx_t *ctx, int argc, char **argv);
void cmd_tdd(app_ctx_t *ctx, int argc, char **argv);
void cmd_plan(app_ctx_t *ctx, int argc, char **argv);
void cmd_implement(app_ctx_t *ctx, int argc, char **argv);
void cmd_dashboard(app_ctx_t *ctx, int argc, char **argv);
void cmd_env(app_ctx_t *ctx, int argc, char **argv);
void cmd_contract(app_ctx_t *ctx, int argc, char **argv);
void cmd_status(app_ctx_t *ctx, int argc, char **argv);
void cmd_hud(app_ctx_t *ctx, int argc, char **argv);
void cmd_export(app_ctx_t *ctx, int argc, char **argv);
void cmd_import(app_ctx_t *ctx, int argc, char **argv);
void cmd_workspace(app_ctx_t *ctx, int argc, char **argv);
/* cli_workspace_serve.c: client-side detached-workspace runner serve loop. */
int cmd_workspace_serve(const char *workspace_id);
void cmd_db(app_ctx_t *ctx, int argc, char **argv);
void cmd_session(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_session_subcmds(void);
/* cmd_session_history.c */
void session_subcmd_show(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_search(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_stats(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_tokens(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_brief(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_start(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_status(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_pause(app_ctx_t *ctx, int argc, char **argv);
void session_subcmd_advance(app_ctx_t *ctx, int argc, char **argv);
void cmd_config(app_ctx_t *ctx, int argc, char **argv);
void cmd_git(app_ctx_t *ctx, int argc, char **argv);
void cmd_doctor(app_ctx_t *ctx, int argc, char **argv);
/* Returns malloc'd JSON with all doctor check results. Caller frees. */
char *doctor_checks_json(void);
void cmd_slop(app_ctx_t *ctx, int argc, char **argv);
void cmd_notify(app_ctx_t *ctx, int argc, char **argv);
void cmd_usage(app_ctx_t *ctx, int argc, char **argv);
void cmd_clarify(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory.c */
void cmd_memory(app_ctx_t *ctx, int argc, char **argv);

/* cmd_index.c */
void cmd_index(app_ctx_t *ctx, int argc, char **argv);

/* cmd_graph.c */
void cmd_graph(app_ctx_t *ctx, int argc, char **argv);

/* cmd_rules.c */
void cmd_rules(app_ctx_t *ctx, int argc, char **argv);
/* cmd_roadmap.c */
void cmd_roadmap(app_ctx_t *ctx, int argc, char **argv);
/* cmd_auto.c */
void cmd_auto(app_ctx_t *ctx, int argc, char **argv);
void cmd_learning(app_ctx_t *ctx, int argc, char **argv);

/* cmd_dogfood.c */
void cmd_dogfood(app_ctx_t *ctx, int argc, char **argv);

/* cmd_identity.c */
void cmd_identity(app_ctx_t *ctx, int argc, char **argv);

/* cmd_onboard.c */
void cmd_onboard(app_ctx_t *ctx, int argc, char **argv);
/* Pure report builder; exposed for tests. Caller owns the returned
 * cJSON object and must cJSON_Delete() it. */
struct cJSON *onboard_build_report(app_ctx_t *ctx, int skip_setup);
/* JSON accessors reused by MCP and dashboard surfaces. Caller owns
 * the returned cJSON object and must cJSON_Delete() it. */
struct cJSON;
struct cJSON *identity_charter_json(const config_t *cfg);
struct cJSON *identity_local_operator_json(void);
struct cJSON *identity_working_profile_json(void);
/* Pure snapshot diff. Caller owns both inputs and the returned
 * report. See cmd_identity.c for the shape. */
struct cJSON *identity_diff_report(struct cJSON *snap_a, struct cJSON *snap_b,
                                   double flip_threshold);
/* Pure snapshot builder; caller owns the returned cJSON. */
struct cJSON *identity_snapshot_build(void);

/* cmd_diagnose.c */
void cmd_diagnose(app_ctx_t *ctx, int argc, char **argv);

/* cmd_guardrails.c */
void cmd_guardrails(app_ctx_t *ctx, int argc, char **argv);

void cmd_feedback_raw(app_ctx_t *ctx, int argc, char **argv);
void cmd_feedback_plus(app_ctx_t *ctx, int argc, char **argv);
void cmd_feedback_minus(app_ctx_t *ctx, int argc, char **argv);

/* cmd_hooks.c */
void cmd_hooks(app_ctx_t *ctx, int argc, char **argv);
void cmd_session_start(app_ctx_t *ctx, int argc, char **argv);
/* Shared body of session-start. CLI uses stdin/stdout; server RPC reads the
 * hook payload off the wire and writes into an open_memstream so it can echo
 * the captured brief back to the thin client. */
void session_start_emit(app_ctx_t *ctx, const char *hook_input, FILE *out);
/* Workspace-independent brief for the remote thin-client SessionStart path
 * (session.brief_assemble). Side-effect-free: build_session_context only, none
 * of session_start_emit's worktree/state/reindex work. */
void session_brief_emit(FILE *out);
void cmd_launch(app_ctx_t *ctx, int argc, char **argv);
void cmd_wrapup(app_ctx_t *ctx, int argc, char **argv);

/* cmd_agent.c */
void cmd_agent(app_ctx_t *ctx, int argc, char **argv);

/* cmd_agent_delegate.c */
void cmd_delegate(app_ctx_t *ctx, int argc, char **argv);
void cmd_delegate_status(app_ctx_t *ctx, int argc, char **argv);
void cmd_verify(app_ctx_t *ctx, int argc, char **argv);

/* cmd_agent_trace.c */
void cmd_dispatch(app_ctx_t *ctx, int argc, char **argv);
void cmd_queue(app_ctx_t *ctx, int argc, char **argv);
void cmd_context(app_ctx_t *ctx, int argc, char **argv);
void cmd_manifest(app_ctx_t *ctx, int argc, char **argv);
void cmd_trace(app_ctx_t *ctx, int argc, char **argv);
void cmd_trajectory(app_ctx_t *ctx, int argc, char **argv);
void cmd_jobs(app_ctx_t *ctx, int argc, char **argv);
void cmd_plans(app_ctx_t *ctx, int argc, char **argv);
void cmd_eval(app_ctx_t *ctx, int argc, char **argv);

/* cmd_work.c */
void cmd_work(app_ctx_t *ctx, int argc, char **argv);

/* cmd_sweep.c — `aimee sweep [project]`: server-side deepening sweep (analysis-only) */
void cmd_sweep(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_work_subcmds(void);
int work_queue_summary(char *buf, size_t cap);

/* Sync pending/claimed work items against the filesystem state of their source
 * proposals. If a proposal has moved from pending/ into done/ or accepted/, the
 * item is marked 'done'. If it moved into deferred/ or rejected/, the item is
 * 'cancelled'. Items whose source file is still present or whose source is not
 * a proposal are left alone.
 *
 * proposals_base is the directory containing pending/, done/, etc. (e.g.
 * "docs/proposals"). Pass NULL to resolve from cwd.
 * Returns 0 on success. out_closed/out_cancelled receive counts (may be NULL). */
int work_sync_proposals(const char *proposals_base, int *out_closed, int *out_cancelled);

/* cmd_job.c */
void cmd_job(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_job_subcmds(void);

/* cmd_cancel.c */
void cmd_cancel(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_cancel_subcmds(void);

/* cmd_rewind.c */
void cmd_rewind(app_ctx_t *ctx, int argc, char **argv);

/* cmd_branch.c */
void cmd_branch(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_branch_subcmds(void);

/* cmd_roles.c */
void cmd_roles(app_ctx_t *ctx, int argc, char **argv);

/* cmd_toolset.c */
void cmd_toolset(app_ctx_t *ctx, int argc, char **argv);

/* cmd_skill.c */
void cmd_skill(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_skill_subcmds(void);

/* cmd_mcp.c */
void cmd_mcp(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_mcp_subcmds(void);

/* cmd_autopilot.c */
void cmd_autopilot(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_autopilot_subcmds(void);

/* cmd_plugin.c */
void cmd_plugin(app_ctx_t *ctx, int argc, char **argv);

/* cmd_run.c */
void cmd_run(app_ctx_t *ctx, int argc, char **argv);

/* cmd_kb.c */
void cmd_kb(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_kb_subcmds(void);

/* cmd_trigger.c */
void cmd_trigger(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_trigger_subcmds(void);

/* cmd_provider.c */
void cmd_provider(app_ctx_t *ctx, int argc, char **argv);
void cmd_model(app_ctx_t *ctx, int argc, char **argv);

/* cmd_aux.c */
void cmd_aux(app_ctx_t *ctx, int argc, char **argv);
void cmd_audit(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_aux_subcmds(void);

/* cmd_infra.c (webchat, gateway) */
void cmd_webchat(app_ctx_t *ctx, int argc, char **argv);
void cmd_gateway(app_ctx_t *ctx, int argc, char **argv);

/* cmd_describe.c */
void cmd_describe(app_ctx_t *ctx, int argc, char **argv);

/* describe_read: read project description file. Caller owns returned string (or NULL). */
char *describe_read(const char *project_name);

/* cmd_wm.c */
void cmd_wm(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_wm_subcmds(void);

/* cmd_table.c: help command */
void cmd_help(app_ctx_t *ctx, int argc, char **argv);
int command_is_alias(const char *name);
int command_is_hidden_default(const char *name);
void print_commands_for_tier(cmd_tier_t tier);

/* Subtable accessors for the help system */
const subcmd_t *get_memory_subcmds(void);
const subcmd_t *get_agent_subcmds(void);
const subcmd_t *get_index_subcmds(void);
const subcmd_t *get_db_subcmds(void);

/* cmd_wiki.c */
void cmd_wiki(app_ctx_t *ctx, int argc, char **argv);

/* Command table (defined in cmd_table.c) */
extern const command_t commands[];

/* Build aimee capabilities reference text. Caller owns the returned string. */
char *build_capabilities_text(void);

int cmd_profile_run(int argc, char **argv);

#endif
