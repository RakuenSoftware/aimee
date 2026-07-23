/* roundtable_types.h: private compatibility names for the delegates-owned panel
 * request and IR-owned result contracts. Required code never includes this
 * optional header. */
#ifndef AIMEE_ROUNDTABLE_TYPES_H
#define AIMEE_ROUNDTABLE_TYPES_H 1

#include <aimee/delegates/panel_provider.h>
#include <aimee/ir/panel_result.h>

#define ROUNDTABLE_MAX_REVIEW_ITEMS AIMEE_PANEL_MAX_REVIEW_ITEMS
#define ROUNDTABLE_MAX_QUESTIONS    AIMEE_PANEL_MAX_QUESTIONS

#define ROUNDTABLE_DRAFT      AIMEE_PANEL_DRAFT
#define ROUNDTABLE_REVIEW     AIMEE_PANEL_REVIEW
#define ROUNDTABLE_PARALLEL   AIMEE_PANEL_PARALLEL
#define ROUNDTABLE_SEQUENTIAL AIMEE_PANEL_SEQUENTIAL

typedef aimee_panel_mode_t roundtable_mode_t;
typedef aimee_panel_turns_t roundtable_turns_t;
typedef aimee_panel_options_t roundtable_opts_t;

/* Private compatibility names for IR-owned message types and bounds. Required
 * code uses the AIMEE_* and aimee_* names. A producing provider allocates the
 * artifact and supplies its release callback, but the caller must release the
 * result exactly once through aimee_panel_result_release before provider
 * unregistration; delegates core dispatches the callback. The only project
 * dependencies here are canonical required-core headers, so dependency remains
 * directed optional -> required. Required code importing this private header is
 * rejected by scripts/check_panel_contract_boundary.py. */
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
