#ifndef DEC_LEARNING_H
#define DEC_LEARNING_H 1

/* Ensemble-learning signal/proposal types. The DB2 SQL layer that persists these
 * lives in db2/db2_learning.h (formerly db2/learning.h; renamed to remove the
 * basename collision with this header, so -Imodules/learning no longer needs a
 * special position relative to -Idb1/-Idb2). */

#include <stdint.h>

#define LEARNING_MAX_PROPOSAL_IDS 8

typedef struct
{
   char signal_type[32];
   char source[16];
   char polarity[16];
   char title[256];
   char description[1024];
   char target_key[256];
   int64_t target_memory_id;
   char correction_text[1024];
   char workflow_project[128];
   char workflow_signal_type[64];
   const char *evidence_refs_json; /* JSON array string, NULL => "[]" */
   int high_confidence;
} learning_signal_input_t;

typedef struct
{
   int id;
   int signal_id;
   char sink[32];
   char state[16];
   char target_key[256];
   int64_t target_memory_id;
   char action_json[2048];
   char evidence_refs[1024];
   int corroboration_count;
   char expires_at[32];
   char committed_at[32];
   char archive_reason[64];
   char created_at[32];
   char updated_at[32];
} learning_proposal_t;

typedef struct
{
   int signal_id;
   int proposal_ids[LEARNING_MAX_PROPOSAL_IDS];
   int proposal_count;
   int committed_ids[LEARNING_MAX_PROPOSAL_IDS];
   int committed_count;
} learning_dispatch_result_t;

struct cJSON;

int learning_router_enabled(void);
int learning_router_record_signal(const learning_signal_input_t *input,
                                  learning_dispatch_result_t *out);
int learning_list_proposals(const char *state, const char *sink, int limit,
                            learning_proposal_t *out, int max);
int learning_get_proposal(int id, learning_proposal_t *out);
int learning_accept_proposal(int id, learning_proposal_t *out);
int learning_reject_proposal(int id, learning_proposal_t *out);
struct cJSON *learning_proposal_to_json(const learning_proposal_t *proposal);
int learning_proposal_from_json(const struct cJSON *obj, learning_proposal_t *out);
struct cJSON *learning_dispatch_result_to_json(const learning_dispatch_result_t *result);

/* --- Phase-2 metrics ---
 * Drift and capacity signals used by operator dashboards and CI.
 * See docs/proposals/done/learning-signals-router-phase-2.md. */

#define LEARNING_METRICS_DEFAULT_WINDOW_DAYS 7

/* Aggregate commit ratio over `window_days` (values <= 0 pick the
 * default 7-day window). Rows in state='committed' / rows in any
 * terminal state (committed or archived), excluding still-pending
 * proposals so the ratio reflects settled decisions.  commit_ratio
 * is 0 when the denominator is zero. Returns 0 on success. */
typedef struct
{
   int64_t proposals_terminal;  /* committed + archived within window */
   int64_t proposals_committed; /* committed within window */
   double commit_ratio;         /* committed / max(1, terminal) */
   int window_days;
} learning_commit_ratio_t;
int learning_metrics_commit_ratio(int window_days, learning_commit_ratio_t *out);

/* Per-sink rolling-week cap utilization. Reports
 * committed_this_week / weekly_cap per sink in the canonical order
 * (reranker, supersede, rule, workflow). `out` must hold at least
 * `max` entries; returns rows written or -1 on error. */
#define LEARNING_SINK_COUNT 4
typedef struct
{
   char sink[32];
   int committed_this_week;
   int weekly_cap;
   double utilization;
} learning_sink_cap_t;
int learning_metrics_per_sink_caps(learning_sink_cap_t *out, int max);

/* Process-local latency telemetry for the signal-ingest path
 * (learning_router_record_signal). Callers not interested in a
 * field can pass NULL. `ingest_ms_max` tracks the slowest call,
 * `ingest_ms_avg` the arithmetic mean across `ingest_calls` samples. */
void learning_router_metrics(int64_t *ingest_calls, double *ingest_ms_avg, double *ingest_ms_max);
void learning_router_metrics_reset(void);

/* Process-local latency telemetry for the implicit-signal detection path
 * (learning_implicit_detect_turn and related detectors). `detection_calls`
 * counts turn-level dispatch invocations (not per-signal fired).
 * Callers not interested in a field can pass NULL. */
void learning_router_detection_metrics(int64_t *detection_calls, double *detection_ms_avg,
                                       double *detection_ms_max);
void learning_router_detection_metrics_reset(void);

/* Called by learning_implicit.c to record a detection-pass timing. */
void learning_router_record_detection_ms(double ms);

#endif
