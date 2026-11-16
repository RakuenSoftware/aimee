/* roundtable_types.h: the plain-data result and options types produced by an
 * ensemble roundtable run and consumed by the roundtable module (chair, verify,
 * pipeline). These were previously defined inside delegate_ensemble.h, which
 * forced the roundtable module's headers to include a delegates header purely for
 * these types — a roundtable -> delegates header edge that, combined with
 * delegates including the roundtable headers, formed a header cycle between the
 * two modules. Owning them here (the roundtable module) breaks that cycle: both
 * delegate_ensemble.h and the roundtable headers now include this leaf header, so
 * the only cross-module edge is delegates -> roundtable.
 *
 * Shared evidence/review/question shapes come from the core data-only header.
 * It has no includes or provider dependencies; keep this header free of runtime
 * and delegates headers so sharing data cannot recreate the provider cycle. */
#ifndef AIMEE_ROUNDTABLE_TYPES_H
#define AIMEE_ROUNDTABLE_TYPES_H 1

#include <aimee/core/types/panel_result.h>

#define ROUNDTABLE_MAX_REVIEW_ITEMS AIMEE_PANEL_MAX_REVIEW_ITEMS
#define ROUNDTABLE_MAX_QUESTIONS    AIMEE_PANEL_MAX_QUESTIONS

typedef enum
{
   ROUNDTABLE_DRAFT = 0,
   ROUNDTABLE_REVIEW = 1
} roundtable_mode_t;

typedef enum
{
   ROUNDTABLE_PARALLEL = 0,
   ROUNDTABLE_SEQUENTIAL = 1
} roundtable_turns_t;

typedef struct
{
   roundtable_mode_t mode;
   roundtable_turns_t turns;
   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   int apply_review;
   const char *brief;
   int brief_truncated;
   /* Optional pre-assembled read-only context (aimee memory recall + code-graph
    * snippets) injected into every panelist prompt — a useful SEED, no longer the
    * only window: REVIEW-mode panelists now run with aimee's index-only toolset
    * (`review_indexed`) and can look things up themselves. Drafting panelists are
    * still tool-less, so for them this remains the only view of memory and the code
    * graph. The caller owns the string for the duration of the run; NULL = none. */
   const char *context;
   const char **questions;
   int question_count;
   int (*cancel_requested)(void *ctx);
   void *cancel_ctx;
   /* Originating session: panel + aggregator runs fold their cost onto it via
    * db1_cost_fold_record, so the ensemble is accounted like a delegate. */
   const char *parent_session_id;
   /* Leading effective-panel seats [0..required_participants) are caller-authored
    * must-use lenses. Later auto-filled seats are best-effort. Zero means every
    * participant is required (legacy/default). Values above panel size fail. */
   int required_participants;
} roundtable_opts_t;

/* Replay-verification evidence (Part A of the replayable-verification proposal).
 * A panelist attaches a STRUCTURED query — never a free-form command — so a fresh
 * verifier can replay it deterministically over the read-only code index
 * (index_find / index_find_callers / index_code_search) and re-ground the claim.
 * Plain (no pointers): memset-zeroing and struct copy stay valid. */
typedef aimee_review_evidence_kind_t ev_kind_t;
#define EV_NONE   AIMEE_REVIEW_EVIDENCE_NONE
#define EV_SYMBOL AIMEE_REVIEW_EVIDENCE_SYMBOL
#define EV_REFS   AIMEE_REVIEW_EVIDENCE_REFS
#define EV_SEARCH AIMEE_REVIEW_EVIDENCE_SEARCH

typedef aimee_review_evidence_t review_evidence_t;
typedef aimee_panel_review_item_t roundtable_review_item_t;
typedef aimee_panel_answered_question_t roundtable_answered_question_t;

typedef struct
{
   char *artifact;
   int rounds_run;
   int converged;
   int degraded;
   int truncated;
   int cost_capped;
   int deadline_hit;
   int cancelled;
   int best_round;
   int participants_total;  /* reference models per round (panel size) */
   int participants_failed; /* unusable responses in the adopted best round */
   /* Failures in the adopted best round's required prefix. This is always <=
    * participants_failed; degraded remains run-level and sticky across rounds. */
   int participants_required_failed;
   int participants_tool_used;            /* adopted review round: seats with >=1 Aimee tool call */
   int participant_tool_calls;            /* adopted review round: total Aimee tool calls */
   int participant_successful_tool_calls; /* adopted review round: non-error results */
   int evidence_coverage_incomplete;      /* evidence gate was on but a responding seat used none */
   double cost_usd;
   /* Explicit assessment of whether the reviewed direction still serves the
    * originating request. `unclear` is fail-closed for workflow gates: a panel
    * that did not compare the work to the ask cannot approve it by omission. */
   char original_request_alignment[16]; /* aligned | drifted | unclear */
   char original_request_alignment_summary[512];
   char original_request_alignment_sources[256];
   roundtable_review_item_t items[ROUNDTABLE_MAX_REVIEW_ITEMS];
   int item_count;
   int items_round;
   int artifact_round;
   roundtable_answered_question_t answered_questions[ROUNDTABLE_MAX_QUESTIONS];
   int answered_question_count;
   char coverage_gaps[ROUNDTABLE_MAX_QUESTIONS][512];
   int coverage_gap_count;
   /* Replay verification (Part A). Items whose structured evidence did not
    * reproduce against the code index are moved here (not silently dropped), with
    * a parallel reason code. Fixed arrays (no heap; no change to result_free). */
   roundtable_review_item_t rejected[ROUNDTABLE_MAX_REVIEW_ITEMS];
   char rejected_reason[ROUNDTABLE_MAX_REVIEW_ITEMS][24];
   int rejected_count;
   int verified_count; /* kept items whose factual evidence reproduced */
   int degraded_count; /* kept but unverified (index unavailable) */
   int capped_count;   /* interpretive items capped at suggestion */
} roundtable_result_t;

/* The ensemble/roundtable settings ONE run needs, as plain data. Previously a
 * legacy_config_record was threaded through the runtime as a value carrier -- mutated in
 * memory to overlay a preset, never persisted. That made every panel function a
 * legacy_config_record consumer for what is really a dozen scalars. Populate with
 * ensemble_panel_from_config(), then overlay a preset if one is requested.
 * Widths match the corresponding config fields exactly. */
#define ENSEMBLE_PANEL_MAX_SEATS 32
typedef struct
{
   char reference_models[ENSEMBLE_PANEL_MAX_SEATS][128];
   char reference_personas[ENSEMBLE_PANEL_MAX_SEATS][64];
   int reference_count;
   int reference_persona_count;
   char aggregator[128];
   int min_successful;
   double max_cost_usd;
   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   int chair_synthesis;
   int require_evidence;
   int replay_verify_enabled;
   char turns[16];
} ensemble_panel_t;

#endif /* AIMEE_ROUNDTABLE_TYPES_H */
