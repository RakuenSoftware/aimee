/* kb_http_grants.h — the kb-side surface for per-user write-tier grant administration
 * (per-user-remote-writes-authz.md increment 5, item 3).
 *
 * §6 of the proposal makes the upgrade fail-closed: a deployment lands with zero grants
 * and denies every remote write until an operator populates them. §7 says the local UDS
 * operator does that populating. The primitives existed (db2_write_tier_grant_*) but
 * nothing exposed them, so the documented procedure had no tool. These routes are the
 * kb half of that tool; aimee-server reaches them on the operator's behalf, because the
 * server links neither DB2 nor libpq and cannot touch the grant table itself.
 *
 * AUTHORIZATION IS NOT HERE, and that is deliberate rather than an omission. Two
 * independent checks apply, established by different machinery:
 *
 *   the DB layer  kb_write_tier_grant_set / _revoke are SECURITY DEFINER and require
 *                 admin or team-lead authority, submitted to the KB WORM outbox. Reads are
 *                 RLS-scoped to the caller's teams. That rule is shipped and tested and
 *                 is NOT tightened here — a stricter copy of an authorization check is
 *                 how a regression enters.
 *   the server    the /v1 routes that call these require a local UDS connection, so a
 *                 remote caller holding a write tier cannot administer grants and widen
 *                 its own access.
 *
 * Neither is the whole rule, and this file adds a third of neither. It validates shapes
 * and marshals JSON.
 */
#ifndef DEC_KB_HTTP_GRANTS_H
#define DEC_KB_HTTP_GRANTS_H

/* Handle a write-tier grant route. Returns the HTTP status if (method, path) is one of
 * them, or -1 if it is not — the same fall-through convention as the other kb route
 * satellites, and the reason an unrelated path reaches the rest of the router.
 *
 * Routes:
 *   POST /v1/write-tier-grants/set     {server_id, team_id, subject, tier}
 *                                      -> {changed, was_revoked, previous_tier?, is_member}
 *   POST /v1/write-tier-grants/revoke  {server_id, team_id, subject}
 *   GET  /v1/write-tier-grants         ?server_id&team_id[&include_revoked=1][&subject]
 *                                      -> {grants:[...]}
 *
 * `show` is deliberately NOT a route of its own: it is the listing filtered to one
 * subject, and a second endpoint returning a subset of the same rows would be a second
 * place for the row shape to drift. The CLI presents it as a separate command.
 */
int kb_http_grants_route(const char *method, const char *path, const char *query_string,
                         const char *body, char *out_buf, int out_cap);

#endif /* DEC_KB_HTTP_GRANTS_H */
