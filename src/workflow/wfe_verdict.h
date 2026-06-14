/* wfe_verdict.h: the roundtable verdict contract + the fail-closed gate decision
 * rule. These types live in the W5 slice (NOT the frozen wfe_iface seam) so they
 * can grow without breaking the W1 contract test. */
#ifndef DEC_WFE_VERDICT_H
#define DEC_WFE_VERDICT_H 1

#include <stddef.h>

#define WFE_VERDICT_SCHEMA 1

typedef enum
{
   WFE_V_APPROVE = 0,
   WFE_V_REQUEST_CHANGES,
   WFE_V_COMMENT,
   WFE_V_MALFORMED /* unparseable/empty -> fail-closed to REQUEST_CHANGES */
} wfe_verdict_kind_t;

typedef struct
{
   char persona[32];
   char model[32];
   int schema_version;
   char reviewed_content_hash[72];
   wfe_verdict_kind_t kind;
   int high_sev_blockers; /* count of unresolved high-severity blockers */
} wfe_verdict_t;

typedef enum
{
   WFE_GATE_APPROVE = 0, /* -> ADVANCED */
   WFE_GATE_CHANGES,     /* -> LOOPED (re-author/re-implement) */
   WFE_GATE_DEGRADED     /* -> PENDING (panel could not be composed) */
} wfe_gate_decision_t;

/* Fail-closed scoring. `artifact_hash` is the lifecycle's hash of the artifact
 * under review. For each verdict: an unknown schema_version, or a
 * reviewed_content_hash != artifact_hash, or MALFORMED kind is coerced to
 * REQUEST_CHANGES (defends against approving a tampered/different artifact).
 * Decision:
 *   - DEGRADED if any required persona is missing a verdict;
 *   - CHANGES  if any unresolved high-severity blocker (any source), or the
 *              APPROVE count (COMMENT does NOT count) is below quorum;
 *   - APPROVE  otherwise.
 * `quorum` is the already-floored effective quorum (caller applies max(2,nreq)).
 */
wfe_gate_decision_t wfe_gate_decide(const wfe_verdict_t *verdicts, int n,
                                    const char *const *required, int nreq, int quorum,
                                    const char *artifact_hash, char *reason, size_t rcap);

/* The effective quorum: max(requested, 2, nreq) — the single-lens floor. */
int wfe_gate_effective_quorum(int requested, int nreq);

#endif /* DEC_WFE_VERDICT_H */
