/* kb_insights_util.h: pure, dependency-light helpers for /v1/insights/spend (P3b).
 *
 * Two testable-in-isolation pieces of the spend reporting surface, kept out of the HTTP
 * TU so the unit test can link them without the db2/tenant/router stack:
 *   - kb_insights_date_valid(): boundary ISO-date validation (the route rejects a
 *     malformed date with 400 BEFORE the definer call; the definer re-validates too);
 *   - kb_insights_spend_json(): assembles the response JSON from the grouped rows,
 *     deriving total + by_model + by_project. cost_usd is summed as an EXACT scale-10
 *     fixed-point decimal (NEVER a C double) and emitted as a JSON string, so the total
 *     reconciles with the per-row costs to the last digit for finance export. */
#ifndef DEC_KB_INSIGHTS_UTIL_H
#define DEC_KB_INSIGHTS_UTIL_H 1

#include <stddef.h>

#include "org_spend.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* 1 iff s is a well-formed AND valid ISO calendar date 'YYYY-MM-DD' (correct month
    * 1-12 and day-of-month, leap years honored); 0 otherwise (NULL, wrong length, junk,
    * or an impossible calendar day like 2026-13-40 / 2026-02-30). */
   int kb_insights_date_valid(const char *s);

   /* Exact sum of scale-10 fixed-point decimal strings (as returned for a NUMERIC(20,10)
    * column). Writes the canonical 'W.FFFFFFFFFF' text (10 fractional digits) to out.
    * No floating point anywhere. Non-negative inputs (costs are non-negative). */
   void kb_insights_cost_sum(const char *const *costs, int n, char *out, size_t cap);

   /* Build the /v1/insights/spend response JSON from the grouped (team, project, model)
    * rows: total + by_team + by_model + by_project. by_team preserves the team dimension
    * (one entry for a team-scoped call, the key breakdown for the org-wide admin report).
    * has_team==0 emits "team":null (org-wide); has_project==1 adds a "project" field.
    * Returns a malloc'd unformatted JSON string (caller frees), or NULL on OOM. */
   char *kb_insights_spend_json(int has_team, long long team, int has_project, long long project,
                                const char *since, const char *until,
                                const db2_org_spend_row_t *rows, int n);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_INSIGHTS_UTIL_H */
