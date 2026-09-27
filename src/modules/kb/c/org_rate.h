/* db2/org_rate.h: P4b keyed fixed-window RPM rate limiter (tiered-llm-p4b).
 *
 * Thin typed C access over the SECURITY DEFINER rate functions in db2/schema.sql
 * (org_rate_check / org_rate_policy_set / org_rate_policy_show). RATE ONLY — the budget
 * reservation core is P4a; the egress WIRING of org_rate_check is P2b (this layer exposes
 * db2_org_rate_check for that, but P4b routes only policy set/show over HTTP). The definer
 * enforces admin/lead authz INTERNALLY; this layer maps the definer's RAISE to a sentinel
 * (42501 'admin only'/'not authorized' -> DENIED) and surfaces the STRUCTURED admission
 * result (admitted, binding_dim, reset_epoch) — never parsed error text (the P4a typed
 * >=1000-refusal convention, in its structured shape). Tenant-scoped: requires the
 * RLS-enforcing Postgres backend. kb-only (rides DB2_SRCS -> KB_DB2_OBJS; never enters
 * aimee-server). */
#ifndef DEC_DB2_ORG_RATE_H
#define DEC_DB2_ORG_RATE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The definer RAISEd SQLSTATE 42501 (admin-gated set by a non-admin, or a show by a
 * non-admin/non-lead). Distinct from -1 so the HTTP route maps it to 403 and every
 * other failure to 500. */
#define DB2_RATE_ERR_DENIED (-2)

/* Bound of a dim_key / window binding string surfaced by org_rate_check. dim_key is
 * '<dim>:<scope>' (e.g. 'team:940001'); leave headroom for a long model / cred_slot ref. */
#define DB2_RATE_DIMKEY_CAP 160

/* One rate policy row, as returned by org_rate_policy_show(). */
typedef struct
{
   int64_t id;
   char dim[16];          /* team | project | cert | model | cred_slot */
   char scope_key[128];   /* the concrete id/name, or '*' for the dim default */
   int64_t window_seconds;
   int64_t max_count;
} db2_org_rate_policy_t;

/* Max policy rows a single show() may carry (an exact (dim, scope) match is 0..1). */
#define DB2_RATE_MAX_ROWS 64

/* The structured org_rate_check outcome (the P2b admission contract). admitted == 1 when
 * every applicable window had headroom (and was incremented); admitted == 0 when a window
 * bound — binding_dim then names the binding dim_key (e.g. 'team:940001') and reset_epoch
 * is that window's next boundary (Unix epoch seconds), a stable retry-after. On admit,
 * binding_dim is "" and reset_epoch is 0. */
typedef struct
{
   int admitted;
   char binding_dim[DB2_RATE_DIMKEY_CAP];
   int64_t reset_epoch;
} db2_org_rate_result_t;

/* Admin-gated upsert of a rate policy (org_rate_policy_set). scope_key '*' sets the dim
 * default. Returns 0 on success (out_id set to the row id when non-NULL),
 * DB2_RATE_ERR_DENIED (not admin), or -1. Must run inside an open tenant scope
 * (aimee.principal read by the admin gate). */
int db2_org_rate_policy_set(const char *dim, const char *scope_key, int64_t window_seconds,
                            int64_t max_count, int64_t *out_id);

/* Actor-bound read of the (dim, scope_key) policy (org_rate_policy_show). Fills out[0..n)
 * and returns n (0..max), DB2_RATE_ERR_DENIED (not admin/lead), or -1. Must run inside an
 * open tenant scope (the admin/lead predicate reads aimee.principal). */
int db2_org_rate_policy_show(const char *dim, const char *scope_key,
                             db2_org_rate_policy_t *out, int max);

/* The atomic keyed fixed-window admission (org_rate_check) — the P2b enforcement
 * primitive (NOT wired to egress in P4b). Takes the RESOLVED IDENTITY: team (required),
 * and optionally project (has_project), model, cred_slot (NULL/empty = absent). On a
 * DB-level outcome returns 0 and fills *out; on a definer error returns -1. Tenant-scoped. */
int db2_org_rate_check(int64_t team, int has_project, int64_t project, const char *model,
                       const char *cred_slot, db2_org_rate_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ORG_RATE_H */
