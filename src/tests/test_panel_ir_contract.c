#include <aimee/ir/panel_result.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(AIMEE_PANEL_MAX_PARTICIPANTS == 32, "panel participant bound changed");
_Static_assert(AIMEE_PANEL_MAX_REVIEW_ITEMS == 128, "panel review-item bound changed");
_Static_assert(AIMEE_PANEL_MAX_QUESTIONS == 16, "panel question bound changed");

static void test_evidence_contract(void)
{
   aimee_review_evidence_t evidence = {0};
   assert(evidence.kind == AIMEE_REVIEW_EVIDENCE_NONE);
   assert(AIMEE_REVIEW_EVIDENCE_SYMBOL == 1);
   assert(AIMEE_REVIEW_EVIDENCE_REFS == 2);
   assert(AIMEE_REVIEW_EVIDENCE_SEARCH == 3);

   evidence.kind = AIMEE_REVIEW_EVIDENCE_REFS;
   evidence.count = 4;
   evidence.factual = 1;
   snprintf(evidence.target, sizeof(evidence.target), "%s", "panel_result");

   aimee_review_evidence_t copy = evidence;
   assert(copy.kind == AIMEE_REVIEW_EVIDENCE_REFS);
   assert(copy.count == 4);
   assert(copy.factual == 1);
   assert(strcmp(copy.target, "panel_result") == 0);
}

static void test_result_contract(void)
{
   char artifact[] = "reviewed artifact";
   aimee_panel_result_t result = {0};
   result.artifact = artifact;
   result.participants_total = AIMEE_PANEL_MAX_PARTICIPANTS;
   result.items[0].evidence.kind = AIMEE_REVIEW_EVIDENCE_SYMBOL;
   result.item_count = 1;
   result.answered_questions[0].answered = 1;
   result.answered_question_count = 1;
   result.coverage_gap_count = AIMEE_PANEL_MAX_QUESTIONS;
   result.rejected_count = AIMEE_PANEL_MAX_REVIEW_ITEMS;

   aimee_panel_result_t copy = result;
   assert(copy.artifact == artifact);
   assert(copy.items[0].evidence.kind == AIMEE_REVIEW_EVIDENCE_SYMBOL);
   assert(copy.answered_questions[0].answered == 1);
   assert(copy.coverage_gap_count == AIMEE_PANEL_MAX_QUESTIONS);
   assert(copy.rejected_count == AIMEE_PANEL_MAX_REVIEW_ITEMS);
   assert(sizeof copy.items / sizeof copy.items[0] == AIMEE_PANEL_MAX_REVIEW_ITEMS);
   assert(sizeof copy.answered_questions / sizeof copy.answered_questions[0] ==
          AIMEE_PANEL_MAX_QUESTIONS);
}

int main(void)
{
   test_evidence_contract();
   test_result_contract();
   puts("panel IR contract: ok");
   return 0;
}
