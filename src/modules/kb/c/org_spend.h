/* db2/org_spend.h: P3b org spend reporting (tiered-llm-p3b). Read-only.
 *
 * Thin typed C access over the SECURITY DEFINER aggregation function org_spend_query()
 * in db2/schema.sql. The definer enforces the admin/lead predicate INTERNALLY (it IS the
 * authz gate — SECURITY DEFINER bypasses RLS), so this layer never re-checks authz; it
 * maps the definer's RAISE to a sentinel (42501 'not authorized' -> DENIED, 22007
 * 'bad date' -> BADDATE, anything else -> -1). cost_usd is carried as a NUMERIC TEXT
 * string end-to-end (aimee_pg_column_text) — NEVER a C double, so finance export loses
 * no precision. Reads the rollup ONLY (no ledger variant). Tenant-scoped: requires the
 * RLS-enforcing Postgres backend. kb-only (rides DB2_SRCS -> KB_DB2_OBJS). */
#ifndef DEC_DB2_ORG_SPEND_H
#define DEC_DB2_ORG_SPEND_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The definer RAISEd SQLSTATE 42501 ('not authorized' — the actor is neither an org
 * admin nor a lead of the requested team). Distinct from -1 so the HTTP route maps it
 * to 403 and every other failure to 500. */
#define DB2_SPEND_ERR_DENIED  (-2)
/* The definer RAISEd a 'bad date' error (malformed/invalid/inverted range). Maps to
 * 400 at the HTTP boundary (which also pre-validates, so this is defense-in-depth). */
#define DB2_SPEND_ERR_BADDATE (-3)
/* The grouped result exceeds DB2_SPEND_MAX_ROWS — the report would not fit the caller's
 * buffer. Returned INSTEAD of silently truncating, so total/by_* always reconcile; the
 * caller must narrow the team/project/date range. Maps to HTTP 413. */
#define DB2_SPEND_ERR_TOOBIG (-4)

/* Max grouped (team, project, model) rows a single report may carry. Chosen large enough
 * for a real org over a wide window; a query that would produce more is a TOOBIG error
 * (never a partial 200). Callers size their row buffer to exactly this. */
#define DB2_SPEND_MAX_ROWS 4096

/* cost_usd text buffer. NUMERIC(20,10) summed over rows: 20 integer + 10 fractional
 * digits + sign + '.' + NUL fits comfortably; sized with headroom. */
#define DB2_SPEND_COST_CAP 64

   /* One grouped (team, project, model) spend row as returned by org_spend_query(). The
    * team_id is preserved so an org-wide (team-absent) admin report stays team-aware —
    * two teams' rows for the same (project, model) never merge. */
   typedef struct
   {
      int64_t team_id;          /* the row's team (always present) */
      int64_t project_id;       /* valid only when has_project == 1 */
      int has_project;          /* 0 when the rollup row's project_id was SQL NULL */
      char billable_model[208];
      int64_t prompt_tokens;
      int64_t completion_tokens;
      int64_t cache_read_tokens;
      int64_t cache_write_tokens;
      char cost_usd[DB2_SPEND_COST_CAP]; /* NUMERIC as text — never a double */
      int64_t calls;
   } db2_org_spend_row_t;

   /* Query authorized spend, grouped per (project, model), over org_spend_rollup for
    * day in [since, until]. When has_team == 0 the org-wide (admin-only) branch is taken
    * (team is ignored); when has_project == 0 the project filter is omitted. since/until
    * are ISO 'YYYY-MM-DD' TEXT. out MUST have room for at least max rows; pass
    * max == DB2_SPEND_MAX_ROWS. Fills out[0..n) and returns n (0..max), or a negative
    * sentinel (DB2_SPEND_ERR_DENIED / DB2_SPEND_ERR_BADDATE / DB2_SPEND_ERR_TOOBIG / -1).
    * TOOBIG means the grouped result exceeds max — the result is NEVER silently truncated,
    * so a returned n is always a COMPLETE set that reconciles. Must run inside an open
    * tenant scope (db2_tenant_scope_begin sets aimee.principal, read by the predicate). */
   int db2_org_spend_query(int has_team, int64_t team, int has_project, int64_t project,
                           const char *since, const char *until, db2_org_spend_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ORG_SPEND_H */
