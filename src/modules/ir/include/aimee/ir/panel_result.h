/* panel_result.h: provider-neutral panel output messages.
 *
 * IR owns the aimee_panel_result_t data layout only. Delegates core owns
 * provider registration, facade invocation, and release dispatch; deliberation
 * policy and execution remain provider-specific. A provider allocates the
 * artifact returned in a successful result and supplies its matching release
 * callback. The caller must invoke aimee_panel_result_release exactly once
 * before the provider is unregistered. A shallow struct copy is a non-owning
 * view of the same allocation and must not be released independently. */
#ifndef AIMEE_IR_PANEL_RESULT_H
#define AIMEE_IR_PANEL_RESULT_H 1

#define AIMEE_PANEL_MAX_PARTICIPANTS 32
#define AIMEE_PANEL_MAX_REVIEW_ITEMS 128
#define AIMEE_PANEL_MAX_QUESTIONS    16

typedef enum
{
   AIMEE_REVIEW_EVIDENCE_NONE = 0,
   AIMEE_REVIEW_EVIDENCE_SYMBOL,
   AIMEE_REVIEW_EVIDENCE_REFS,
   AIMEE_REVIEW_EVIDENCE_SEARCH
} aimee_review_evidence_kind_t;

typedef struct
{
   aimee_review_evidence_kind_t kind;
   char target[256];
   char project[128];
   int count;
   char idkey[65];
   int factual;
} aimee_review_evidence_t;

typedef struct
{
   char severity[16];
   char category[32];
   char location[128];
   char summary[256];
   char recommendation[256];
   char identity_key[128];
   char sources[256];
   int count;
   int tool_grounded;
   aimee_review_evidence_t evidence;
} aimee_panel_review_item_t;

typedef struct
{
   char question[512];
   char answer[1024];
   char evidence[512];
   int answered;
} aimee_panel_answered_question_t;

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
   int participants_total;
   int participants_failed;
   double cost_usd;
   aimee_panel_review_item_t items[AIMEE_PANEL_MAX_REVIEW_ITEMS];
   int item_count;
   int items_round;
   int artifact_round;
   aimee_panel_answered_question_t answered_questions[AIMEE_PANEL_MAX_QUESTIONS];
   int answered_question_count;
   char coverage_gaps[AIMEE_PANEL_MAX_QUESTIONS][512];
   int coverage_gap_count;
   aimee_panel_review_item_t rejected[AIMEE_PANEL_MAX_REVIEW_ITEMS];
   char rejected_reason[AIMEE_PANEL_MAX_REVIEW_ITEMS][24];
   int rejected_count;
   int verified_count;
   int degraded_count;
   int capped_count;
   char original_request_alignment[16];
   char original_request_alignment_summary[512];
   char original_request_alignment_sources[256];
} aimee_panel_result_t;

#endif /* AIMEE_IR_PANEL_RESULT_H */
