#ifndef DEC_LEARNING_H
#define DEC_LEARNING_H 1

/* Ensemble-learning signal/proposal types. The DB2 SQL layer that persists these
 * lives in db2/db2_learning.h (formerly db2/learning.h; renamed to remove the
 * basename collision with this header, so -Imodules/learning no longer needs a
 * special position relative to -Idb1/-Idb2). */

#include <stdint.h>

typedef int (*learning_signal_classifier_fn)(const char *signal_type, uint32_t *sink_mask);
/* Production installs the supervised event-bus classifier during server startup.
 * NULL clears it. Signal ingestion fails before persistence when it is absent or fails. */
void learning_router_register_signal_classifier(learning_signal_classifier_fn classifier);

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

/* --- Endogeneity accounting (recursive-self-improvement S0) ---
 *
 * A learning loop that feeds on its own output drifts into an echo chamber:
 * it keeps committing proposals whose only evidence is something it inferred
 * earlier. commit_ratio cannot see that — it measures acceptance, not where
 * the evidence came from. These functions classify committed proposals by the
 * provenance of the signal that raised them and expose the exogenous share as
 * a gate.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

typedef enum
{
   /* Rooted in something outside the system's own reasoning: a human acting
    * through the feedback surface, a test/verify exit status, a grader, a git
    * outcome, an operator accept. */
   LEARNING_EVIDENCE_EXOGENOUS = 0,
   /* Rooted in the system's own output: an implicit detector reading aimee's
    * own transcript, a lesson derived from a lesson, a self-generated eval
    * result. Unknown provenance classifies here — the conservative side. */
   LEARNING_EVIDENCE_ENDOGENOUS = 1,
} learning_evidence_origin_t;

/* Classify a signal's provenance from its (source, signal_type) pair. Pure;
 * no DB access. `signal_type` overrides `source` for the implicit-detector
 * types, because the signal-capture API defaults an unset source to
 * "explicit" — without that override a caller could launder a self-derived
 * signal into the exogenous count by simply omitting the field. NULL or empty
 * arguments classify as endogenous. */
learning_evidence_origin_t learning_evidence_classify(const char *source, const char *signal_type);

/* Exogenous share of committed proposals over a window. `exogenous_ratio` is
 * meaningful only when `committed_total` > 0; it is 0.0 for an empty window,
 * which means "nothing observed", NOT "wholly endogenous" — read
 * committed_total before acting on the ratio. */
typedef struct
{
   int64_t committed_total;      /* committed proposals in the window */
   int64_t committed_exogenous;  /* of those, exogenously rooted */
   int64_t committed_endogenous; /* of those, endogenously rooted */
   double exogenous_ratio;       /* exogenous / max(1, total); 0 when total == 0 */
   int window_days;
} learning_endogeneity_t;

/* Overall (sink == NULL / "") or per-sink endogeneity over `window_days`
 * (values <= 0 pick LEARNING_METRICS_DEFAULT_WINDOW_DAYS). Returns 0 on
 * success, -1 on bad args / SQL / connection error. */
int learning_metrics_endogeneity(int window_days, learning_endogeneity_t *out);
int learning_metrics_endogeneity_for_sink(int window_days, const char *sink,
                                          learning_endogeneity_t *out);

/* Floor below which self-generated signal is judged to dominate, and the
 * sample size below which the ratio is not yet worth acting on. A fresh
 * installation has committed nothing, so the gate must not be closed by an
 * empty window — that would make it impossible to ever bootstrap. */
#define LEARNING_ENDOGENEITY_MIN_EXOGENOUS_RATIO 0.25
#define LEARNING_ENDOGENEITY_MIN_SAMPLE          20

typedef enum
{
   LEARNING_GATE_OPEN = 0,
   /* Observed a large enough sample and the exogenous share is under the
    * floor: the loop is feeding on itself. Gated promotions must refuse. */
   LEARNING_GATE_CLOSED_ENDOGENOUS = 1,
   /* The share could not be computed (DB2 configured but erroring). Distinct
    * from CLOSED so the caller can decide; a build with DB2 compiled out
    * reports OPEN instead, since no learning is being persisted there. */
   LEARNING_GATE_UNAVAILABLE = 2,
} learning_gate_state_t;

/* Gate state over the window. `out` may be NULL. learning_gate_check() uses
 * the defaults above; the _with() form takes explicit bounds so a caller (and
 * the tests) can supply their own without a config round trip. */
learning_gate_state_t learning_gate_check(learning_endogeneity_t *out);
learning_gate_state_t learning_gate_check_with(int window_days, double min_exogenous_ratio,
                                               int min_sample, learning_endogeneity_t *out);

#endif
