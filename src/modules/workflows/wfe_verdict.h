/* wfe_verdict.h: the roundtable verdict contract + the fail-closed gate decision
 * rule. These types live in the W5 slice (NOT the frozen wfe_iface seam) so they
 * can grow without breaking the W1 contract test. */
#ifndef DEC_WFE_VERDICT_H
#define DEC_WFE_VERDICT_H 1

#include <stddef.h>

#define WFE_VERDICT_SCHEMA 1

/* Max length of a panelist's captured critique (the reasoning that precedes the
 * final JSON verdict line). Threaded back to the re-authoring delegate on a
 * request_changes so the planner refines against the actual objections. */
#define WFE_VERDICT_FEEDBACK_MAX 1024

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
   int high_sev_blockers;                   /* count of unresolved high-severity blockers */
   char feedback[WFE_VERDICT_FEEDBACK_MAX]; /* the panelist's critique text (reasoning before
                                             * the JSON verdict line); may be empty */
} wfe_verdict_t;

typedef enum
{
   WFE_GATE_APPROVE = 0, /* -> ADVANCED */
   WFE_GATE_CHANGES,     /* -> LOOPED (re-author/re-implement) */
   WFE_GATE_DEGRADED     /* -> PENDING (panel could not be composed) */
} wfe_gate_decision_t;

#endif /* DEC_WFE_VERDICT_H */
