/* roundtable_types.h: optional roundtable policy plus private compatibility
 * names for the canonical IR panel-result messages. Required code includes
 * <aimee/ir/panel_result.h> directly and never depends on this private header. */
#ifndef AIMEE_ROUNDTABLE_TYPES_H
#define AIMEE_ROUNDTABLE_TYPES_H 1

#include <aimee/ir/panel_result.h>

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
} roundtable_opts_t;

/* Private compatibility names for IR-owned message types and bounds. Required
 * code uses the AIMEE_* and aimee_* names. These aliases do not change artifact
 * ownership: the producing provider's release operation frees it exactly once. */
#define EV_NONE   AIMEE_REVIEW_EVIDENCE_NONE
#define EV_SYMBOL AIMEE_REVIEW_EVIDENCE_SYMBOL
#define EV_REFS   AIMEE_REVIEW_EVIDENCE_REFS
#define EV_SEARCH AIMEE_REVIEW_EVIDENCE_SEARCH

typedef aimee_review_evidence_kind_t ev_kind_t;
typedef aimee_review_evidence_t review_evidence_t;
typedef aimee_panel_review_item_t roundtable_review_item_t;
typedef aimee_panel_answered_question_t roundtable_answered_question_t;
typedef aimee_panel_result_t roundtable_result_t;

#endif /* AIMEE_ROUNDTABLE_TYPES_H */
