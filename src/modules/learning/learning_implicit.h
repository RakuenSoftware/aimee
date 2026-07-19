#ifndef DEC_LEARNING_IMPLICIT_H
#define DEC_LEARNING_IMPLICIT_H 1

#include <stdint.h>

/* Called once per user turn, after dogfood_autolabel_next_turn_live().
 * Emits citation_then_repair (polarity=negative) or citation_then_continuation
 * (polarity=positive) implicit learning signals when the respective
 * learning_implicit_citation_repair / learning_implicit_citation_continuation
 * config flags are on. No-op when both flags are off. */
void learning_implicit_detect_turn(const char *user_text);

/* Called from the dogfood write path when the (session, tool, query_hash)
 * triple has already appeared this month. Emits a repeat_question implicit
 * signal when learning_implicit_repeat_question is on. */
void learning_implicit_record_repeat_question(const char *session_id, const char *tool,
                                              const char *query_hash);

/* Called after recording a correction signal. Emits a repeated_correction
 * implicit signal when learning_implicit_repeated_correction is on. */
void learning_implicit_record_correction(const char *target_key, int64_t target_memory_id);

/* Called after kb_client_memory_upsert_workflow() succeeds. Emits a
 * workflow_repetition implicit signal when learning_implicit_workflow_repetition
 * is on. */
void learning_implicit_record_workflow(const char *workspace, const char *signal_type,
                                       const char *description);

#endif /* DEC_LEARNING_IMPLICIT_H */
