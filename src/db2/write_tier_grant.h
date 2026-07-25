/* db2/write_tier_grant.h — per-user /v1 write authorization (proposal
 * per-user-remote-writes-authz.md §6). Backed by kb_write_tier_grant: the
 * authoritative {subject -> tier} map within a (server, team).
 *
 * Fail-closed is the whole point of this module. "No live grant" is a distinct,
 * explicitly-signalled outcome — never a tier value, never folded into an error
 * code, and never defaulted to `off`. A caller that cannot tell "granted off"
 * from "not granted" would be correct today and wrong the moment `off` gains a
 * meaning, so the API refuses to conflate them.
 *
 * Tenant-scoped: every entry requires the RLS-enforcing Postgres backend
 * (db2_tenant_require_pg), matching db2/admin_grant.c.
 *
 * The tier ENUM is shared with the token layer (kb_identity_tier_t), but the
 * tier<->string mapping is deliberately local. db2 objects link into BOTH the
 * server and kb, while kb/kb_identity_token.c (the token *builder*) links into
 * kb only. Calling the builder's mapping here would drag the minting path into
 * the server binary, which is exactly the separation the server-side verifier
 * maintains by carrying its own copy. Types are free across that boundary;
 * functions are not. */
#ifndef DEC_DB2_WRITE_TIER_GRANT_H
#define DEC_DB2_WRITE_TIER_GRANT_H 1

#include "kb_identity_token.h" /* kb_identity_tier_t (type only — see above) */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      char subject[577];
      kb_identity_tier_t tier;
      char granted_by[577];
      char created_at[32];
      char updated_at[32];
   } db2_write_tier_grant_row_t;

   enum
   {
      /* A live grant exists; *out holds its tier. */
      DB2_WRITE_TIER_GRANT_FOUND = 1,
      /* No live grant for this (server, team, subject). The caller MUST deny.
       * This is the post-migration default for every subject. */
      DB2_WRITE_TIER_GRANT_NONE = 0
   };

   /* Resolve the live tier granted to `subject` within (server_id, team_id).
    * Writes *out only on FOUND; zeroes it otherwise.
    *
    * Returns DB2_WRITE_TIER_GRANT_FOUND, DB2_WRITE_TIER_GRANT_NONE, or a
    * NEGATIVE error — a negative tenancy code (so the blanket shim guard in
    * test_kb_tenancy_shim_guard.c sees the same DB2_ERR_TENANT_REQUIRES_PG as
    * every other tenant-scoped entrypoint), or -1.
    *
    * All three deny. They stay distinguishable so an outage is never recorded
    * as a policy decision, and so "granted off" never reads as "not granted". */
   int db2_write_tier_grant_lookup(const char *server_id, int64_t team_id, const char *subject,
                                   kb_identity_tier_t *out);

   /* Insert or update the grant. Idempotent on (server_id, team_id, subject): a
    * re-grant refreshes tier/granted_by and clears revoked_at. Returns 0, a
    * negative tenancy code, or -1. */
   int db2_write_tier_grant_set(const char *server_id, int64_t team_id, const char *subject,
                                kb_identity_tier_t tier, const char *granted_by);

   /* Revoke the live grant. Idempotent — revoking an absent or already-revoked
    * grant succeeds. Returns 0, a negative tenancy code, or -1. */
   int db2_write_tier_grant_revoke(const char *server_id, int64_t team_id, const char *subject);

   /* List live grants for (server_id, team_id), ordered by subject. Writes up to
    * `cap` rows and the total written to *count. Returns 0, a negative tenancy
    * code, or -1. */
   int db2_write_tier_grant_list(const char *server_id, int64_t team_id,
                                 db2_write_tier_grant_row_t *out, size_t cap, size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_WRITE_TIER_GRANT_H */
