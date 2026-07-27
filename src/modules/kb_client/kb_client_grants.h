/* kb_client_grants.h — aimee-server's client for kb's write-tier grant routes
 * (per-user-remote-writes-authz.md increment 5, item 4).
 *
 * The server cannot touch kb_write_tier_grant directly: it links neither DB2 nor libpq,
 * which scripts/check_tier_deps.sh enforces. The operator CLI's grant commands therefore
 * arrive at the server and the server asks kb. This declares that ask.
 *
 * No policy lives behind this header. Two independent checks apply, and both are
 * elsewhere: the server's /v1 routes require a LOCAL UDS connection, and kb's SQL requires
 * admin or team-lead authority with a WORM audit row in the same transaction.
 */
#ifndef DEC_KB_CLIENT_GRANTS_H
#define DEC_KB_CLIENT_GRANTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Distinct outcomes, because an operator's next action differs for each and folding
    * them into one failure would send people to check the wrong thing. */
   typedef enum
   {
      KB_CLIENT_GRANT_OK = 0,
      /* Malformed request — a client bug or a bad argument, not a permissions problem. */
      KB_CLIENT_GRANT_INVALID,
      /* kb refused: no admin or team-lead authority, or the (server, team) pair is not
       * registered. */
      KB_CLIENT_GRANT_DENIED,
      /* kb cannot administer grants on this backend at all — RLS is unenforceable on the
       * SQLite shim, so this is a deployment fault rather than an authorization one. */
      KB_CLIENT_GRANT_BACKEND,
      /* kb was unreachable, timed out, or answered something unusable. Deliberately NOT
       * merged with DENIED: reporting an unreachable kb as a refusal is a lie about
       * authority, and would have an operator editing grants that were never consulted. */
      KB_CLIENT_GRANT_UNAVAILABLE
   } kb_client_grant_result_t;

   /* What a set replaced, as kb observed it atomically. */
   typedef struct
   {
      int changed;      /* the row was created, re-tiered, or un-revoked */
      int was_revoked;  /* a revocation was cleared by this call */
      int had_previous; /* 0 when the grant did not exist; previous_tier is then unset */
      char previous_tier[16];
      int is_member; /* 0 when the subject is not on the team: the grant is inert until
                      * they are, which is a warning and not a refusal */
   } kb_client_grant_change_t;

   typedef struct
   {
      char subject[578];
      char tier[16];
      char granted_by[578];
      char created_at[32];
      char updated_at[32];
      char revoked_at[32]; /* empty for a live grant */
   } kb_client_grant_row_t;

   /* Create or update the grant, reporting what it replaced. `tier` is "off", "data" or
    * "full" — validated by kb, which owns that vocabulary. */
   kb_client_grant_result_t kb_client_grant_set(const char *server_id, int64_t team_id,
                                                const char *subject, const char *tier,
                                                const char *granted_by,
                                                kb_client_grant_change_t *out);

   /* Revoke the grant. Idempotent. *found_out is 0 when no grant existed at all — likely a
    * typo'd subject, and distinct from one that was already revoked, where the operator's
    * intent is already satisfied. */
   kb_client_grant_result_t kb_client_grant_revoke(const char *server_id, int64_t team_id,
                                                   const char *subject, int *found_out);

   /* List grants for (server_id, team_id). `subject` is an optional filter — that is how
    * `show` is served, so the row shape has one definition. `include_revoked` WIDENS the
    * listing to contain revoked grants alongside live ones; it is not a revoked-only
    * filter.
    *
    * *truncated_out is set when the answer is partial, whether because kb hit its own
    * ceiling or because `cap` did. A caller must report that rather than present a partial
    * grant list as the complete picture of who can write. */
   kb_client_grant_result_t kb_client_grant_list(const char *server_id, int64_t team_id,
                                                 const char *subject, int include_revoked,
                                                 kb_client_grant_row_t *out, size_t cap,
                                                 size_t *count, int *truncated_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CLIENT_GRANTS_H */
