/* kb_identity_resolve.c: DB-backed composite identity resolution (slice 2, I7).
 *
 * For each present principal it runs an identity-bootstrap scope — sets
 * aimee.principal (team = 0) so the own-rows RLS policy exposes that principal's
 * memberships — reads its team set + default, then applies the pure fail-closed
 * kb_identity_combine. Any lookup failure yields an EMPTY team set for that
 * principal (deny), so the combine denies fail-closed rather than widening. */

#include "kb_identity.h"

#include "modules/db2/c/db2_tenant.h"
#include "membership.h"

#include <string.h>

/* Read one principal's own teams + default under an identity-bootstrap scope.
 * On any failure leaves *n = 0 / *deflt = 0 (deny). */
static void lookup_principal(const kb_principal_t *p, int64_t *teams, int *n, int64_t *deflt)
{
   *n = 0;
   *deflt = 0;
   char key[600];
   if (kb_identity_key(p, key, sizeof(key)) != 0)
      return;
   /* Bootstrap: set aimee.principal only (team 0), so the own-rows policy on
    * kb_team_membership exposes exactly this principal's rows. */
   if (db2_tenant_scope_begin(p, 0) != 0)
      return;
   int cnt = db2_membership_teams(key, teams, KB_MAX_TEAMS);
   if (cnt > 0)
      *n = cnt;
   (void)db2_membership_default_team(key, deflt); /* leaves *deflt=0 when none */
   db2_tenant_scope_rollback();                   /* read-only unit; clears the GUCs */
}

kb_resolve_status_t kb_identity_resolve(const kb_principal_t *transport,
                                        const kb_principal_t *actor, int64_t named_team,
                                        kb_request_context_t *out)
{
   if (!out)
      return KB_RESOLVE_CONFLICT;
   memset(out, 0, sizeof(*out));

   int has_t = transport && transport->authenticated;
   int has_a = actor && actor->authenticated;

   int64_t tteams[KB_MAX_TEAMS], ateams[KB_MAX_TEAMS];
   int n_t = 0, n_a = 0;
   int64_t tdefault = 0, adefault = 0;

   if (has_t)
   {
      lookup_principal(transport, tteams, &n_t, &tdefault);
      out->transport = *transport;
      out->has_transport = 1;
   }
   if (has_a)
   {
      lookup_principal(actor, ateams, &n_a, &adefault);
      out->actor = *actor;
      out->has_actor = 1;
   }

   return kb_identity_combine(tteams, n_t, tdefault, has_t, ateams, n_a, adefault, has_a,
                              named_team, out->teams, &out->n_teams, &out->billing_team);
}
