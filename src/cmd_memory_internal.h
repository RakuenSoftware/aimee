/* cmd_memory_internal.h: shared among the cmd_memory_* translation units
 * (cmd_memory.c + cmd_memory_core.c + cmd_memory_embed.c + cmd_memory_vector.c).
 * Not a public API. */
#ifndef DEC_CMD_MEMORY_INTERNAL_H
#define DEC_CMD_MEMORY_INTERNAL_H 1

#include "aimee.h"
#include "agent_eval.h"
#include "commands.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "memory_ontology.h"

/* File-scope config, loaded once by cmd_memory before dispatch. */
extern config_t s_mem_cfg;

/* Env-knob readers and option-shape helpers used by the subcommand handlers. */
double cmd_memory_env_weight(const char *name, double fallback);
void cmd_memory_apply_rerank_mode(const opt_parsed_t *opts);
const char *cmd_memory_scope_type(const opt_parsed_t *opts);
const char *cmd_memory_scope_value(const opt_parsed_t *opts);
void cmd_memory_build_filter(const opt_parsed_t *opts, memory_filter_t *out);
int cmd_memory_find_facts(const opt_parsed_t *opts, const char *query, int limit, memory_t *facts,
                          int max);
int cmd_memory_diagnose_query(const opt_parsed_t *opts, const char *query, int limit,
                              memory_diagnostic_t *rows, int max);
int cmd_memory_ask(const opt_parsed_t *opts, const char *query, int limit,
                   memory_answer_result_t *out);
void cmd_memory_require_runtime(int rc, const char *op);

/* Formats the per-score breakdown into a JSON object (defined in
 * cmd_memory_embed.c alongside the explain/search handlers that produce it). */
cJSON *memory_score_parts_to_json(const memory_score_parts_t *parts);

/* Eval-JSON emitters (cmd_memory_embed.c). */
void mem_emit_eval_json(app_ctx_t *ctx, const char *suite, const char *dataset,
                        const mem_eval_scores_t *scores, const mem_eval_latency_t *latency,
                        const char *weight_profile);
void mem_emit_eval_qa_json(app_ctx_t *ctx, const char *suite, const char *dataset,
                           const mem_eval_qa_scores_t *scores, const mem_eval_latency_t *latency,
                           const char *weight_profile);

/* Subcommand handlers — each is a subcmd_t entry in `memory_subcmds[]`
 * (cmd_memory.c). Grouped by file for grep-ability; the split is purely for
 * file-size management, not a functional boundary. */

/* cmd_memory_core.c — CRUD, stats, task/decide, link/tag */
void mem_store(app_ctx_t *ctx, int argc, char **argv);
void mem_get(app_ctx_t *ctx, int argc, char **argv);
void mem_delete(app_ctx_t *ctx, int argc, char **argv);
void mem_approve(app_ctx_t *ctx, int argc, char **argv);
void mem_list(app_ctx_t *ctx, int argc, char **argv);
void mem_search(app_ctx_t *ctx, int argc, char **argv);
void mem_plan(app_ctx_t *ctx, int argc, char **argv);
void mem_stats(app_ctx_t *ctx, int argc, char **argv);
void mem_scan(app_ctx_t *ctx, int argc, char **argv);
void mem_queue(app_ctx_t *ctx, int argc, char **argv);
void mem_drain(app_ctx_t *ctx, int argc, char **argv);
void mem_edges(app_ctx_t *ctx, int argc, char **argv);
void mem_compact(app_ctx_t *ctx, int argc, char **argv);
void mem_conflicts(app_ctx_t *ctx, int argc, char **argv);
void mem_health(app_ctx_t *ctx, int argc, char **argv);
void mem_provenance(app_ctx_t *ctx, int argc, char **argv);
void mem_maintain(app_ctx_t *ctx, int argc, char **argv);
void mem_briefing(app_ctx_t *ctx, int argc, char **argv);
void mem_remind(app_ctx_t *ctx, int argc, char **argv);
void mem_reminders(app_ctx_t *ctx, int argc, char **argv);
void mem_alerts(app_ctx_t *ctx, int argc, char **argv);
void mem_recall(app_ctx_t *ctx, int argc, char **argv);
void mem_directive(app_ctx_t *ctx, int argc, char **argv);
void mem_directives(app_ctx_t *ctx, int argc, char **argv);
void mem_task(app_ctx_t *ctx, int argc, char **argv);
void mem_decide(app_ctx_t *ctx, int argc, char **argv);
void mem_decisions(app_ctx_t *ctx, int argc, char **argv);
void mem_antipattern(app_ctx_t *ctx, int argc, char **argv);
void mem_supersede(app_ctx_t *ctx, int argc, char **argv);
void mem_history(app_ctx_t *ctx, int argc, char **argv);
void mem_checkpoint(app_ctx_t *ctx, int argc, char **argv);
void mem_style(app_ctx_t *ctx, int argc, char **argv);
void mem_read(app_ctx_t *ctx, int argc, char **argv);
void mem_link(app_ctx_t *ctx, int argc, char **argv);
void mem_mlink(app_ctx_t *ctx, int argc, char **argv);
void mem_mlinks(app_ctx_t *ctx, int argc, char **argv);
void mem_munlink(app_ctx_t *ctx, int argc, char **argv);
void mem_tag(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory_lint.c — memory lint surface */
void mem_lint(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory_embed.c — embed, reembed, diagnose, answer, eval suite */
void mem_embed(app_ctx_t *ctx, int argc, char **argv);
void mem_reembed(app_ctx_t *ctx, int argc, char **argv);
void mem_diagnose(app_ctx_t *ctx, int argc, char **argv);
void mem_answer(app_ctx_t *ctx, int argc, char **argv);
void mem_reflect(app_ctx_t *ctx, int argc, char **argv);
void mem_audit(app_ctx_t *ctx, int argc, char **argv);
void mem_calibrate(app_ctx_t *ctx, int argc, char **argv);
void mem_benchmark(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory_vector.c — reindex/repair/rebuild/reconcile/verify + workflows */
void mem_reindex(app_ctx_t *ctx, int argc, char **argv);
void mem_repair(app_ctx_t *ctx, int argc, char **argv);
void mem_reconcile(app_ctx_t *ctx, int argc, char **argv);
void mem_rebuild(app_ctx_t *ctx, int argc, char **argv);
void mem_verify(app_ctx_t *ctx, int argc, char **argv);
void mem_profile(app_ctx_t *ctx, int argc, char **argv);
void mem_improve(app_ctx_t *ctx, int argc, char **argv);
void mem_cognify(app_ctx_t *ctx, int argc, char **argv);
void mem_episode(app_ctx_t *ctx, int argc, char **argv);
void mem_assemble(app_ctx_t *ctx, int argc, char **argv);
void mem_cite(app_ctx_t *ctx, int argc, char **argv);
void mem_scene(app_ctx_t *ctx, int argc, char **argv);
void mem_ontology(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory_op.c — typed mutation verbs (memory public contract) */
void mem_op(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory_pack.c — profile pack management */
void mem_pack(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory_curiosity.c — curiosity backlog */
void mem_curiosity(app_ctx_t *ctx, int argc, char **argv);

/* cmd_review.c — charter artifact review surface */
void mem_review(app_ctx_t *ctx, int argc, char **argv);

#endif /* DEC_CMD_MEMORY_INTERNAL_H */
