#ifndef DEC_LEARNING_IMPLICIT_H
#define DEC_LEARNING_IMPLICIT_H 1

#include <stdint.h>

/* Called from the dogfood write path when the (session, tool, query_hash)
 * triple has already appeared this month. Emits a repeat_question implicit
 * signal when learning_implicit_repeat_question is on. */
void learning_implicit_record_repeat_question(const char *session_id, const char *tool,
                                              const char *query_hash);

/* Called after recording a correction signal. Emits a repeated_correction
 * implicit signal when learning_implicit_repeated_correction is on. */
void learning_implicit_record_correction(const char *target_key, int64_t target_memory_id);

/* Called after the memory owner accepts a successful write receipt. Emits a
 * workflow_repetition implicit signal when learning_implicit_workflow_repetition
 * is on. */
void learning_implicit_record_workflow(const char *workspace, const char *signal_type,
                                       const char *description);

/* The accepted memory owner's opaque evidence references remain separate from
 * the legacy numeric target_memory_id. NULL preserves the historical [] form.
 * Invalid/oversized arrays refuse emission rather than losing provenance. */
void learning_implicit_record_workflow_evidence(const char *workspace, const char *signal_type,
                                                const char *description,
                                                const char *evidence_refs_json);

#endif /* DEC_LEARNING_IMPLICIT_H */
