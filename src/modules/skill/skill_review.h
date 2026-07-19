#ifndef DEC_SKILL_REVIEW_H
#define DEC_SKILL_REVIEW_H 1

/* Returns 1 if a skill review delegate should fire for this hook_call_count. */
int skill_review_should_fire(int hook_call_count, int nudge_interval);

#endif /* DEC_SKILL_REVIEW_H */
