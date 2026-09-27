/* db2/org_budget.h: P4a budget reservation core (tiered-llm-p4a).
 *
 * Thin typed C access over the SECURITY DEFINER budget functions in db2/schema.sql
 * (org_budget_reserve/_settle/_settle_expired/_heartbeat/_set/_show). BUDGET ONLY —
 * the keyed rate limiter is deferred to P4b. Money is carried as NUMERIC TEXT strings
 * end-to-end (aimee_pg_*_text) — NEVER a C double — so a hard cap is exact. The definer
 * enforces admin/lead authz INTERNALLY; this layer maps the definer's RAISE to a
 * sentinel (42501 'admin only'/'not authorized' -> DENIED, 23505 replay-mismatch ->
 * CONFLICT, 23514 retroactive-reduction -> RETRO) and parses the reserve refusal string.
 * Tenant-scoped: requires the RLS-enforcing Postgres backend. kb-only (rides DB2_SRCS
 * -> KB_DB2_OBJS and never enters aimee-server). */
#ifndef DEC_DB2_ORG_BUDGET_H
#define DEC_DB2_ORG_BUDGET_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The definer RAISEd SQLSTATE 42501 (admin-gated write by a non-admin, or a show by a
 * non-admin/non-lead). Distinct from -1 so the HTTP route maps it to 403 and every
 * other failure to 500. */
#define DB2_BUDGET_ERR_DENIED (-2)
/* org_budget_reserve replay: a same-(origin,request) re-reserve whose immutable triple
 * (team, project, pricing_version, reserved_max) differs from the bound row (23505). */
#define DB2_BUDGET_ERR_CONFLICT (-3)
/* org_budget_set rejected a retroactive reduction of the limit below the current
 * period's committed (spend + reserved) (23514). Maps to HTTP 409. */
#define DB2_BUDGET_ERR_RETRO (-4)

/* reserve() outcome. GRANTED = admitted; the REFUSED_* map the typed 'refused:<reason>'
 * strings the definer returns (a refusal is never persisted). A negative db2 sentinel is
 * returned separately (see db2_org_budget_reserve). */
#define DB2_BUDGET_GRANTED         0
#define DB2_BUDGET_REFUSED_TEAM    1 /* refused:team budget exceeded */
#define DB2_BUDGET_REFUSED_PROJECT 2 /* refused:project budget exceeded */

/* NUMERIC(20,10) as text: 20 integer + 10 fractional + sign + '.' + NUL, with headroom. */
#define DB2_BUDGET_USD_CAP 64

/* One budget row joined to its current-period counter, as returned by org_budget_show(). */
typedef struct
{
   int64_t team_id;
   int64_t project_id; /* valid only when has_project == 1 */
   int has_project;    /* 0 when the budget row's project_id was SQL NULL (team-wide cap) */
   char period[8];     /* 'day' | 'month' */
   char period_id[16]; /* current UTC period instance ('YYYY-MM-DD' | 'YYYY-MM') */
   char limit_usd[DB2_BUDGET_USD_CAP];
   char soft_limit_usd[DB2_BUDGET_USD_CAP]; /* "" when unset (SQL NULL) */
   char spend_usd[DB2_BUDGET_USD_CAP];
   char reserved_usd[DB2_BUDGET_USD_CAP];
   char remaining_usd[DB2_BUDGET_USD_CAP];
} db2_org_budget_row_t;

/* Max budget rows a single show() may carry (team-wide + per-project, day + month). */
#define DB2_BUDGET_MAX_ROWS 512

/* Admin-gated upsert of a cap (org_budget_set). has_project == 0 sets the team-wide cap.
 * limit_usd / soft_limit_usd are NUMERIC decimal strings; soft may be NULL/"" for unset.
 * Returns 0 on success (out_id set to the row id when non-NULL), DB2_BUDGET_ERR_DENIED
 * (not admin), DB2_BUDGET_ERR_RETRO (retroactive reduction), or -1. Must run inside an
 * open tenant scope (aimee.principal read by the admin gate). */
int db2_org_budget_set(int64_t team, int has_project, int64_t project, const char *period,
                       const char *limit_usd, const char *soft_limit_usd, int64_t *out_id);

/* Actor-bound read of a team's caps + current-period counters (org_budget_show).
 * has_project == 0 shows every cap for the team; else only project's rows. Fills
 * out[0..n) and returns n (0..max), DB2_BUDGET_ERR_DENIED (not admin/lead), or -1. Must
 * run inside an open tenant scope (the admin/lead predicate reads aimee.principal). */
int db2_org_budget_show(int64_t team, int has_project, int64_t project,
                        db2_org_budget_row_t *out, int max);

/* Atomic hard-cap admission (org_budget_reserve) — the P2b reserve-before-dispatch
 * primitive. reserved_max is a NUMERIC decimal string (a conservative ceiling). On a
 * DB-level outcome returns 0 and sets *out_outcome to DB2_BUDGET_GRANTED /
 * _REFUSED_TEAM / _REFUSED_PROJECT; on a definer error returns DB2_BUDGET_ERR_CONFLICT
 * (mismatched replay) or -1. Tenant-scoped. */
int db2_org_budget_reserve(const char *origin_cn, const char *request_id, int64_t team,
                           int has_project, int64_t project, int64_t pricing_version,
                           const char *reserved_max, int64_t lease_ttl_secs, int *out_outcome);

/* Reconcile a reservation (org_budget_settle) — the P2b settle-after-response primitive.
 * realized_usd is a NUMERIC decimal string; the definer charges LEAST(realized,
 * reserved_max). Returns 0 with *out_settled = 1 if this call performed the transition
 * (or a downward correction), 0 with *out_settled = 0 for an idempotent no-op, or -1. */
int db2_org_budget_settle(const char *origin_cn, const char *request_id,
                          const char *realized_usd, int *out_settled);

/* Renew an admitted lease (org_budget_heartbeat). Returns 0 with *out_ok = 1 iff a live
 * admitted lease was renewed, else *out_ok = 0; -1 on error. */
int db2_org_budget_heartbeat(const char *origin_cn, const char *request_id,
                             int64_t lease_ttl_secs, int *out_ok);

/* Sweep expired leases, charging each at reserved_max (org_budget_settle_expired).
 * Returns the number settled (>= 0), or -1 on error. */
int db2_org_budget_settle_expired(int64_t *out_settled);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ORG_BUDGET_H */
