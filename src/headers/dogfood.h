#ifndef DEC_DOGFOOD_H
#define DEC_DOGFOOD_H 1

/* Dogfood qualitative logging: fire-and-forget JSONL records of
 * memory-adjacent tool calls so operators can review retrieval quality
 * over weeks of real use. See docs/proposals/pending/dogfood-agent-eval.md.
 *
 * Records are written to `<log_dir>/YYYY-MM.jsonl`. The path is taken
 * from `dogfood_log_dir` in config, defaulting to `<output_dir>/dogfood`.
 * When `dogfood_commit_raw` is 0 (default), only a short non-cryptographic
 * hash of the query is persisted so the log can be committed to the repo
 * without leaking private text. With `dogfood_commit_raw=1` the raw query
 * is persisted too. */

#include <stddef.h>
#include <stdint.h>

struct config;

typedef struct dogfood_config
{
   int enabled;
   char log_dir[512];
   int commit_raw;
   int inline_tagging;
   int autolabel_repair;
   int autolabel_continuation;
   int autolabel_repeat_question;
} dogfood_config_t;

/* Post-hoc or auto-applied labels. Any field may be NULL / 0 to leave
 * unset. context_richness is 1..5 when present and 0 otherwise. */
typedef struct dogfood_label
{
   const char *outcome;  /* hit / partial / miss / hallucination, or NULL */
   int context_richness; /* 0 = unset, 1..5 otherwise */
   int surprise;         /* 0 or 1 */
   int has_surprise;     /* field is written only when this is non-zero */
   const char *notes;
   int prospective_surfaced; /* 1 when record captures a prospective reminder firing */
} dogfood_label_t;

/* Copy the dogfood-related fields from a full legacy_config_record into a small
 * struct that the logger can consume without pulling in the whole config
 * definition. Safe to call with cfg==NULL (fills defaults). */
void dogfood_config_current(dogfood_config_t *out);

/* Append one record. Fire-and-forget: failures are swallowed and counted
 * via dogfood_metrics(). `tool` is required; the other pointers may be
 * NULL. `retrieved_ids` may be NULL if retrieved_count == 0. Returns
 * a 16-byte stable record id (writes up to 16 chars + NUL into
 * `record_id_out` when non-NULL) so auto-labellers can tag the record
 * they just wrote. */
void dogfood_log_moment(const dogfood_config_t *cfg, const char *tool, const char *query,
                        const int64_t *retrieved_ids, int retrieved_count, const char *notes);

void dogfood_log_moment_with_id(const dogfood_config_t *cfg, const char *tool, const char *query,
                                const int64_t *retrieved_ids, int retrieved_count,
                                const char *notes, char *record_id_out, size_t record_id_cap);

/* Convenience wrapper that loads the current config and invokes
 * dogfood_log_moment(). Callers use this from hot paths to get
 * runtime-togglable logging without carrying the config through. */
void dogfood_log_moment_live(const char *tool, const char *query, const int64_t *retrieved_ids,
                             int retrieved_count, const char *notes);

/* Variant that also writes a label entry into YYYY-MM.labels.jsonl at
 * the same time, keyed by the record's id. Useful for automated
 * tagging paths (e.g. a prospective-memory reminder firing → record
 * surprise=true, prospective_surfaced=true at write-time). */
void dogfood_log_moment_tagged(const char *tool, const char *query, const int64_t *retrieved_ids,
                               int retrieved_count, const dogfood_label_t *label);

/* Append a label entry (post-hoc tagging). cfg==NULL loads the live
 * config. Returns 0 on success, -1 on any failure (also bumps the
 * failure counter). */
int dogfood_label_record(const dogfood_config_t *cfg, const char *record_id,
                         const dogfood_label_t *label);

/* Read YYYY-MM.jsonl and YYYY-MM.labels.jsonl from `dir` and return
 * the merged records as a cJSON array. Labels overwrite matching
 * record fields. Caller takes ownership and must cJSON_Delete(). */
struct cJSON;
struct cJSON *dogfood_read_month(const char *dir, const char *month);

/* Build a monthly-aggregate cJSON report from a records array. Pure
 * function: does not touch the filesystem, so it's directly testable
 * against a fixture array. Caller takes ownership of the returned
 * report object. */
struct cJSON *dogfood_build_report(const struct cJSON *records);

/* Parse a markdown fragment that contains a `## Dogfood Signals` block
 * with `- confirm:` / `- contradict:` predicate lines. Returns a cJSON
 * array of {kind, predicates, count} objects, or NULL if no Signals
 * block is present. Each predicate line is
 *     key=value[, key=value[, count>=N]]
 * joined by AND. Unknown keys pass through untouched so future record
 * fields can be matched without a header change. */
struct cJSON *dogfood_parse_signals_md(const char *markdown);

/* Evaluate a parsed signals array against a records array and return
 * a classification ("confirmed" / "contradicted" / "no-signal"). If
 * both confirm and contradict fire, contradict wins — the monthly
 * report should surface the disconfirming evidence. Return value is a
 * static string; do not free. */
const char *dogfood_classify(const struct cJSON *records, const struct cJSON *signals);

/* Process-lifetime counters for the operator surfaces. Either out may be
 * NULL to skip. */
void dogfood_metrics(int64_t *records_out, int64_t *failures_out);

/* Reset counters (test-only). */
void dogfood_metrics_reset(void);

/* Return an allocated JSON string with a one-shot tagging hint for
 * client UIs to render after a memory-tool call, or NULL when
 * inline_tagging is disabled. The blob carries the record id and
 * the set of valid outcome values. Caller owns and frees. */
char *dogfood_inline_hint_json(const dogfood_config_t *cfg, const char *record_id,
                               const char *tool);

/* --- Weak auto-labelling heuristics ---
 * See docs/proposals/done/dogfood-autolabel-and-first-cycle.md.
 * Each heuristic is a pure function; wiring callers decide when to
 * invoke them (e.g. per-turn for REPAIR/CONTINUATION, per-write for
 * REPEAT). The gate lives in dogfood_config_t.autolabel_* flags. */

typedef enum
{
   DOGFOOD_AUTOLABEL_NONE = 0,
   DOGFOOD_AUTOLABEL_REPAIR,       /* correction cue → outcome=miss */
   DOGFOOD_AUTOLABEL_CONTINUATION, /* advancing turn → outcome=hit */
} dogfood_autolabel_kind_t;

/* Classify the user's next turn after a memory-tool record. REPAIR
 * if the trimmed text starts with a correction cue ("no", "actually",
 * "that's wrong", "not quite", "wrong", "incorrect"), case-insensitive;
 * CONTINUATION if the text is substantive (>= 3 chars after trim and
 * not a cue); NONE otherwise. NULL / empty / whitespace-only text
 * returns NONE. */
dogfood_autolabel_kind_t dogfood_classify_next_turn(const char *next_user_text);

/* Apply an auto-label against a record id based on the kind. Writes
 * outcome=miss (REPAIR) or outcome=hit with surprise=false
 * (CONTINUATION), plus notes="autolabel: <kind>". NONE is a no-op.
 * Labels carry autolabel_source so the report can distinguish
 * operator tags from auto-labels. Returns 0 on success (or NONE /
 * flag-off no-op), -1 on failure. */
int dogfood_autolabel_apply(const dogfood_config_t *cfg, const char *record_id,
                            dogfood_autolabel_kind_t kind);

/* Live variant: reads the most recent record id written in this
 * process and applies REPAIR / CONTINUATION against it, then clears
 * the stored id so a single user turn can label at most one prior
 * record. Gated by dogfood_autolabel_repair /
 * dogfood_autolabel_continuation config keys. Safe to call from hot
 * paths — silently returns when no prior id is stored or both flags
 * are off. */
void dogfood_autolabel_next_turn_live(const char *next_user_text);

/* Scan the current month's log for an earlier record with a matching
 * (session_id, tool, query_hash) triple. Returns 1 on hit, 0 on miss
 * or any failure. Intended for write-time use by dogfood_log_moment;
 * cost is one file read per write so it's gated by the config flag. */
int dogfood_query_is_repeat(const dogfood_config_t *cfg, const char *session_id, const char *tool,
                            const char *query_hash);

/* Build a month report: reads JSONL, computes stats, arms a review reminder
 * via the KB service, and adds month/log_dir fields.
 * dir==NULL resolves from config; month==NULL uses the current calendar month
 * (UTC). Caller owns the returned cJSON or NULL on error. */
struct cJSON *dogfood_build_report_for_month(const char *dir, const char *month);

#endif /* DEC_DOGFOOD_H */
