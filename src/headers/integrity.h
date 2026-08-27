/* integrity.h: Layer 1 deterministic pattern gate for inbound content.
 *
 * Runs before writes enter durable storage (KB normalize, learning
 * substrate evidence, memory mutations).  Source label controls which
 * categories apply and the maximum possible verdict.
 *
 * See docs/proposals/accepted/ingest-poison-gate.md. */
#ifndef INTEGRITY_H
#define INTEGRITY_H

typedef enum
{
   INTEGRITY_SOURCE_USER_STATED = 0,
   INTEGRITY_SOURCE_WEB,
   INTEGRITY_SOURCE_DOCUMENT,
   INTEGRITY_SOURCE_TOOL,
   INTEGRITY_SOURCE_DELEGATE,
   INTEGRITY_SOURCE_AGENT_MESSAGE,
} integrity_source_t;

typedef enum
{
   INTEGRITY_VERDICT_ACCEPT = 0,
   INTEGRITY_VERDICT_QUARANTINE,
   INTEGRITY_VERDICT_REJECT,
   INTEGRITY_VERDICT_REVIEW_NEEDED,
} integrity_verdict_t;

typedef struct
{
   integrity_verdict_t verdict;
   char match_category[32]; /* empty when verdict == ACCEPT */
} integrity_result_t;

/*
 * Run Layer 1 pattern gate on text.
 *
 * Rules:
 *  - MEMORY_RESET / IDENTITY_OVERRIDE / AUTHORITY_CLAIM /
 *    INSTRUCTION_INJECTION patterns are "block" severity:
 *      source != user_stated → REJECT
 *      source == user_stated → QUARANTINE  (never auto-reject user input)
 *  - ENCODED_PAYLOAD patterns are "warn" severity → QUARANTINE always.
 *  - Returns ACCEPT when text is NULL, empty, or no pattern fires.
 *  - The verdict is the actual detection result; callers check their own
 *    dry_run flag and act (or skip acting) accordingly.
 */
integrity_result_t integrity_gate_check(const char *text, integrity_source_t source);

const char *integrity_source_name(integrity_source_t source);
const char *integrity_verdict_name(integrity_verdict_t verdict);

/* The one materialization boundary used by autonomous content ingress. It
 * classifies, emits a durable content-free verdict, and returns nonzero when
 * the caller must park/refuse the content before any write or prompt effect. */
int integrity_ingress_decide(const char *text, integrity_source_t source, const char *boundary,
                             int autonomous, integrity_result_t *result_out);

#endif /* INTEGRITY_H */
