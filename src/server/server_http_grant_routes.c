/* server_http_grant_routes.c — /v1 write-tier grant administration handlers.
 *
 * In their own translation unit for the reason server_http_config_routes.c gives: the route
 * table's TU has a 2500-line ceiling enforced by `make line-check`, and these handlers push
 * it over. The route ROWS stay in the table; only the handlers live here.
 *
 * These reach kb over kb_client, because the server links neither DB2 nor libpq and cannot
 * touch kb_write_tier_grant itself (scripts/check_tier_deps.sh enforces that).
 *
 * REACHABLE ONLY OVER THE LOCAL UDS LISTENER. v1_route_requires_uds refuses them over TCP
 * regardless of bearer, tier or capability — necessary because a remote_writes=full bearer
 * holds CAPS_ALL and would satisfy any capability gate. CAP_GRANT_ADMIN is defence in depth
 * on top of that, not the primary control, and kb independently requires admin or team-lead
 * authority with a WORM audit row. Three checks, established by three different mechanisms.
 */
#include "cJSON.h"
#include "kb_client_grants.h"
#include "server.h"
#include "server_http.h"
#include "server_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── write-tier grant administration (increment 5) ─────────────────────────
 * These reach kb over kb_client, because the server links neither DB2 nor libpq and cannot
 * touch kb_write_tier_grant itself.
 *
 * Reachable ONLY over the local UDS listener: v1_route_requires_uds refuses them over TCP
 * regardless of bearer, tier or capability, because a remote_writes=full bearer holds
 * CAPS_ALL and would otherwise satisfy any capability gate. CAP_GRANT_ADMIN is defence in
 * depth on top of that, not the primary control. kb independently requires admin or
 * team-lead authority. */

/* An integer team_id from JSON. cJSON stores every number as a double, so a bare range
 * check accepts 910001.9 and the cast makes it 910001 — and team_id selects the
 * authorization scope, so a truncation administers a different team's grants. */
static int grant_team_id(const cJSON *v, int64_t *out)
{
   *out = 0;
   if (!cJSON_IsNumber(v))
      return -1;
   double d = v->valuedouble;
   if (!(d >= 1.0) || d >= 9007199254740992.0)
      return -1;
   int64_t n = (int64_t)d;
   if ((double)n != d)
      return -1;
   *out = n;
   return 0;
}

/* kb's outcomes, each to the status an operator can act on. UNAVAILABLE must not become 403
 * — telling somebody they lack authority when kb was simply unreachable sends them to edit
 * grants that were never consulted. */
static int grant_status(kb_client_grant_result_t rc, char *resp, int cap)
{
   switch (rc)
   {
   case KB_CLIENT_GRANT_OK:
      return 200;
   case KB_CLIENT_GRANT_INVALID:
      return err_json(resp, cap, 400, "invalid grant request");
   case KB_CLIENT_GRANT_DENIED:
      return err_json(resp, cap, 403,
                      "kb refused: grant administration requires admin or team-lead "
                      "authority, and the (server, team) pair must be registered");
   case KB_CLIENT_GRANT_BACKEND:
      return err_json(resp, cap, 503, "grant administration requires kb on the postgres backend");
   case KB_CLIENT_GRANT_UNAUTHENTICATED:
      /* Names the server's own missing enrollment. Reported as 502 rather than 401 so it is
       * never read as the CALLER's credential being at fault: the caller reached this route
       * over the local UDS, and it is this server that kb refused. */
      return err_json(resp, cap, 502,
                      "kb refused this server's credentials: it has no kb client identity. "
                      "Complete the AIMEE_KB_CONN enrollment and restart the server");
   case KB_CLIENT_GRANT_UNAVAILABLE:
   default:
      return err_json(resp, cap, 503, "kb is unreachable; no grant was changed");
   }
}

int rh_grant_set(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   const cJSON *jsrv = body ? cJSON_GetObjectItemCaseSensitive(body, "server_id") : NULL;
   const cJSON *jteam = body ? cJSON_GetObjectItemCaseSensitive(body, "team_id") : NULL;
   const cJSON *jsub = body ? cJSON_GetObjectItemCaseSensitive(body, "subject") : NULL;
   const cJSON *jtier = body ? cJSON_GetObjectItemCaseSensitive(body, "tier") : NULL;
   int64_t team_id = 0;
   int ok = cJSON_IsString(jsrv) && jsrv->valuestring && jsrv->valuestring[0] &&
            grant_team_id(jteam, &team_id) == 0 && cJSON_IsString(jsub) && jsub->valuestring &&
            jsub->valuestring[0] && cJSON_IsString(jtier) && jtier->valuestring;
   if (!ok)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400,
                      "server_id, an integer team_id, subject and tier are required");
   }
   /* granted_by is sent for wire compatibility but kb IGNORES it: kb derives the granter from
    * the principal its own verifier produced for the request. That is the stronger rule — a
    * value travelling in a body can be written to order, and kb should not have to trust this
    * server's word about who acted. Kept as "owner" so the field is never absent. */
   kb_client_grant_change_t change;
   kb_client_grant_result_t rc = kb_client_grant_set(jsrv->valuestring, team_id, jsub->valuestring,
                                                     jtier->valuestring, "owner", &change);
   /* The response echoes the subject, so copy it out before the body that owns
    * that string is freed. Reading jsub after the delete is a use-after-free
    * that presents as an empty echo, not a crash. */
   char subject[256];
   snprintf(subject, sizeof(subject), "%s", jsub->valuestring);
   cJSON_Delete(body);
   if (rc != KB_CLIENT_GRANT_OK)
      return grant_status(rc, resp, cap);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "changed", change.changed);
   cJSON_AddBoolToObject(out, "was_revoked", change.was_revoked);
   if (change.had_previous)
      cJSON_AddStringToObject(out, "previous_tier", change.previous_tier);
   cJSON_AddBoolToObject(out, "is_member", change.is_member);
   /* Echo what this grant was for. is_member=false means the grant does nothing
    * until the subject joins, and the remedy is a command on another binary that
    * needs exactly these two values; without them the client can only gesture at
    * it. Additive — an older client ignores them. */
   cJSON_AddNumberToObject(out, "team_id", (double)team_id);
   cJSON_AddStringToObject(out, "subject", subject);
   char *text = cJSON_PrintUnformatted(out);
   int n = text ? snprintf(resp, (size_t)cap, "%s", text) : -1;
   free(text);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

int rh_grant_revoke(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   const cJSON *jsrv = body ? cJSON_GetObjectItemCaseSensitive(body, "server_id") : NULL;
   const cJSON *jteam = body ? cJSON_GetObjectItemCaseSensitive(body, "team_id") : NULL;
   const cJSON *jsub = body ? cJSON_GetObjectItemCaseSensitive(body, "subject") : NULL;
   int64_t team_id = 0;
   int ok = cJSON_IsString(jsrv) && jsrv->valuestring && jsrv->valuestring[0] &&
            grant_team_id(jteam, &team_id) == 0 && cJSON_IsString(jsub) && jsub->valuestring &&
            jsub->valuestring[0];
   if (!ok)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "server_id, an integer team_id and subject are required");
   }
   int found = 0;
   kb_client_grant_result_t rc =
       kb_client_grant_revoke(jsrv->valuestring, team_id, jsub->valuestring, &found);
   cJSON_Delete(body);
   if (rc != KB_CLIENT_GRANT_OK)
      return grant_status(rc, resp, cap);
   /* `found` is reported rather than turned into a 404: revocation is idempotent, so a
    * retry after a timeout must succeed, but an operator who typo'd a subject needs to know
    * nothing was there. */
   int n = snprintf(resp, (size_t)cap, "{\"found\":%s}", found ? "true" : "false");
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* Bounded to what one response can carry. Reported as truncated rather than silently short:
 * a partial grant list read as complete understates who can write. */
#define GRANT_LIST_CAP 256

int rh_grant_list(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   const cJSON *jsrv = body ? cJSON_GetObjectItemCaseSensitive(body, "server_id") : NULL;
   const cJSON *jteam = body ? cJSON_GetObjectItemCaseSensitive(body, "team_id") : NULL;
   const cJSON *jsub = body ? cJSON_GetObjectItemCaseSensitive(body, "subject") : NULL;
   const cJSON *jrev = body ? cJSON_GetObjectItemCaseSensitive(body, "include_revoked") : NULL;
   int64_t team_id = 0;
   if (!cJSON_IsString(jsrv) || !jsrv->valuestring || !jsrv->valuestring[0] ||
       grant_team_id(jteam, &team_id) != 0)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "server_id and an integer team_id are required");
   }
   /* An optional filter: this is how `show` is served, so the row shape has exactly one
    * definition rather than a second endpoint returning a subset of the same rows. */
   int have_subject = cJSON_IsString(jsub) && jsub->valuestring && jsub->valuestring[0];
   /* Only an explicit true widens. Anything else must not read as "yes" on a parameter that
    * reveals revoked history. */
   int include_revoked = cJSON_IsTrue(jrev);

   kb_client_grant_row_t *rows = calloc(GRANT_LIST_CAP, sizeof(*rows));
   if (!rows)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 500, "out of memory");
   }
   size_t count = 0;
   int truncated = 0;
   kb_client_grant_result_t rc =
       kb_client_grant_list(jsrv->valuestring, team_id, have_subject ? jsub->valuestring : NULL,
                            include_revoked, rows, GRANT_LIST_CAP, &count, &truncated);
   cJSON_Delete(body);
   if (rc != KB_CLIENT_GRANT_OK)
   {
      free(rows);
      return grant_status(rc, resp, cap);
   }
   cJSON *out = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(out, "grants");
   for (size_t i = 0; arr && i < count; i++)
   {
      cJSON *g = cJSON_CreateObject();
      cJSON_AddStringToObject(g, "subject", rows[i].subject);
      cJSON_AddStringToObject(g, "tier", rows[i].tier);
      cJSON_AddStringToObject(g, "granted_by", rows[i].granted_by);
      cJSON_AddStringToObject(g, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(g, "updated_at", rows[i].updated_at);
      if (rows[i].revoked_at[0])
         cJSON_AddStringToObject(g, "revoked_at", rows[i].revoked_at);
      cJSON_AddItemToArray(arr, g);
   }
   cJSON_AddBoolToObject(out, "truncated", truncated);
   free(rows);
   char *text = arr ? cJSON_PrintUnformatted(out) : NULL;
   int n = text ? snprintf(resp, (size_t)cap, "%s", text) : -1;
   free(text);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}
