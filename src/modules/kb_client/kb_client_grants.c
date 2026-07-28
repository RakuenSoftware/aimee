/* kb_client_grants.c — the server's client for kb's write-tier grant routes
 * (per-user-remote-writes-authz.md increment 5, item 4).
 *
 * aimee-server cannot touch kb_write_tier_grant itself: it links neither DB2 nor libpq,
 * enforced by scripts/check_tier_deps.sh. So the operator CLI's grant commands arrive at
 * the server, and the server asks kb. This is that ask, and nothing more — no policy
 * lives here.
 *
 * WHERE THE TWO CHECKS ARE, since neither is in this file:
 *   the server  the /v1 routes that call these require a local UDS connection, so a
 *               remote caller holding a write tier cannot administer grants and widen its
 *               own access.
 *   kb          the SQL is SECURITY DEFINER and requires admin or team-lead authority,
 *               with a WORM audit row in the same transaction.
 *
 * A status is returned rather than folded into a bare -1 because kb distinguishes "this
 * backend cannot do grants at all" (503) from "you may not" (403) from "your request was
 * malformed" (400), and an operator needs to know which.
 */
#include "kb_client_grants.h"

#include "cJSON.h"
#include "kb_client_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Grant administration is interactive and touches one row, so it should answer promptly
 * or be reported as unavailable; it must not inherit an indexing-scale timeout. */
#define GRANTS_TIMEOUT_MS 15000

/* Map kb's HTTP status onto the client result. `status_out` is 0 when the request never
 * reached kb at all, which is distinct from every answer kb could give. */
static kb_client_grant_result_t result_from_status(int status)
{
   switch (status)
   {
   case 200:
      return KB_CLIENT_GRANT_OK;
   case 400:
      return KB_CLIENT_GRANT_INVALID;
   case 401:
      /* kb answered, so it is neither unreachable nor unusable: it rejected this server's
       * credential. Falling through to UNAVAILABLE sent operators to debug kb's health and
       * the network while the actual fault — an unenrolled server — went unnamed. */
      return KB_CLIENT_GRANT_UNAUTHENTICATED;
   case 403:
      return KB_CLIENT_GRANT_DENIED;
   case 405:
      /* A method mismatch is a client bug, not an operator's mistake, and must not read as
       * a refusal — that would send somebody to check their authority. */
      return KB_CLIENT_GRANT_INVALID;
   case 503:
      return KB_CLIENT_GRANT_BACKEND;
   default:
      return KB_CLIENT_GRANT_UNAVAILABLE;
   }
}

kb_client_grant_result_t kb_client_grant_set(const char *server_id, int64_t team_id,
                                             const char *subject, const char *tier,
                                             const char *granted_by, kb_client_grant_change_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!server_id || !server_id[0] || team_id < 1 || !subject || !subject[0] || !tier || !tier[0] ||
       !granted_by || !granted_by[0])
      return KB_CLIENT_GRANT_INVALID;

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return KB_CLIENT_GRANT_UNAVAILABLE;
   cJSON_AddStringToObject(req, "server_id", server_id);
   cJSON_AddNumberToObject(req, "team_id", (double)team_id);
   cJSON_AddStringToObject(req, "subject", subject);
   cJSON_AddStringToObject(req, "tier", tier);
   cJSON_AddStringToObject(req, "granted_by", granted_by);

   int status = 0;
   char *json =
       kb_client_v1_post_json("/v1/write-tier-grants/set", req, GRANTS_TIMEOUT_MS, &status);
   cJSON_Delete(req);
   kb_client_grant_result_t rc = result_from_status(status);
   if (!json)
   {
      /* No body. On a FAILURE status the status is itself the information, so it decides.
       * On a 200 it is not a success this caller can report: kb always sends a body on
       * success, and `changed` would be unknown — printing "unchanged" for a mutation that
       * happened is exactly the misreport this whole struct exists to prevent. And with no
       * status at all the request never arrived, which must never read as DENIED. */
      if (!status)
         return KB_CLIENT_GRANT_UNAVAILABLE;
      return rc == KB_CLIENT_GRANT_OK ? KB_CLIENT_GRANT_UNAVAILABLE : rc;
   }
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (rc == KB_CLIENT_GRANT_OK && out)
   {
      const cJSON *changed = cJSON_GetObjectItemCaseSensitive(resp, "changed");
      const cJSON *revoked = cJSON_GetObjectItemCaseSensitive(resp, "was_revoked");
      const cJSON *previous = cJSON_GetObjectItemCaseSensitive(resp, "previous_tier");
      const cJSON *member = cJSON_GetObjectItemCaseSensitive(resp, "is_member");
      /* A 200 whose body does not carry `changed` is not a success this caller can report
       * on: it would print "unchanged" for a mutation that happened. */
      if (!cJSON_IsBool(changed))
         rc = KB_CLIENT_GRANT_UNAVAILABLE;
      else
      {
         out->changed = cJSON_IsTrue(changed);
         out->was_revoked = cJSON_IsTrue(revoked);
         out->is_member = cJSON_IsTrue(member);
         /* Absent means the grant did not exist. "Created" and "changed from off" are
          * different facts, so the flag is what distinguishes them, not an empty string. */
         if (cJSON_IsString(previous) && previous->valuestring)
         {
            out->had_previous = 1;
            snprintf(out->previous_tier, sizeof(out->previous_tier), "%s", previous->valuestring);
         }
      }
   }
   if (rc != KB_CLIENT_GRANT_OK && out)
      memset(out, 0, sizeof(*out));
   cJSON_Delete(resp);
   return rc;
}

kb_client_grant_result_t kb_client_grant_revoke(const char *server_id, int64_t team_id,
                                                const char *subject, int *found_out)
{
   if (found_out)
      *found_out = 0;
   if (!server_id || !server_id[0] || team_id < 1 || !subject || !subject[0])
      return KB_CLIENT_GRANT_INVALID;

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return KB_CLIENT_GRANT_UNAVAILABLE;
   cJSON_AddStringToObject(req, "server_id", server_id);
   cJSON_AddNumberToObject(req, "team_id", (double)team_id);
   cJSON_AddStringToObject(req, "subject", subject);

   int status = 0;
   char *json =
       kb_client_v1_post_json("/v1/write-tier-grants/revoke", req, GRANTS_TIMEOUT_MS, &status);
   cJSON_Delete(req);
   kb_client_grant_result_t rc = result_from_status(status);
   if (!json)
   {
      /* See the note in _set: a 200 without a body is unusable, not a success. */
      if (!status)
         return KB_CLIENT_GRANT_UNAVAILABLE;
      return rc == KB_CLIENT_GRANT_OK ? KB_CLIENT_GRANT_UNAVAILABLE : rc;
   }
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (rc == KB_CLIENT_GRANT_OK)
   {
      const cJSON *found = cJSON_GetObjectItemCaseSensitive(resp, "found");
      /* A BOOLEAN `found` IS REQUIRED on a success. The first version defaulted a missing or
       * non-boolean field to 0, reasoning that "did not exist" was the safe default — but
       * `found: false` is not a safe default, it is an AUTHORITATIVE CLAIM that no grant was
       * there, which an operator acts on by going to look for a typo. Turning an unusable
       * protocol response into that claim is worse than admitting the answer is unknown.
       * A review caught this, and caught that my own test had codified `{}` as a success. */
      if (!cJSON_IsBool(found))
         rc = KB_CLIENT_GRANT_UNAVAILABLE;
      else if (found_out)
         *found_out = cJSON_IsTrue(found);
   }
   if (rc != KB_CLIENT_GRANT_OK && found_out)
      *found_out = 0;
   cJSON_Delete(resp);
   return rc;
}

kb_client_grant_result_t kb_client_grant_list(const char *server_id, int64_t team_id,
                                              const char *subject, int include_revoked,
                                              kb_client_grant_row_t *out, size_t cap, size_t *count,
                                              int *truncated_out)
{
   if (count)
      *count = 0;
   if (truncated_out)
      *truncated_out = 0;
   if (!server_id || !server_id[0] || team_id < 1 || !out || !cap || !count)
      return KB_CLIENT_GRANT_INVALID;
   memset(out, 0, cap * sizeof(*out));

   /* The subject is escaped: it may legitimately contain a percent-encoded colon (the
    * oidc:<iss>:<sub> form), and passing that through unescaped would corrupt the query. */
   char *escaped = NULL;
   if (subject && subject[0])
   {
      escaped = kb_client_query_escape(subject);
      if (!escaped)
         return KB_CLIENT_GRANT_UNAVAILABLE;
   }
   char path[1024];
   int n = snprintf(path, sizeof(path), "/v1/write-tier-grants?server_id=%s&team_id=%lld%s%s%s",
                    server_id, (long long)team_id, include_revoked ? "&include_revoked=1" : "",
                    escaped ? "&subject=" : "", escaped ? escaped : "");
   free(escaped);
   /* Refused rather than truncated: a shortened query would silently drop the subject
    * filter and list every grant on the server. */
   if (n <= 0 || (size_t)n >= sizeof(path))
      return KB_CLIENT_GRANT_INVALID;

   int status = 0;
   char *json = kb_client_v1_get_json(path, GRANTS_TIMEOUT_MS, &status);
   kb_client_grant_result_t rc = result_from_status(status);
   if (!json)
   {
      /* See the note in _set. An empty body is not an empty grant list — reporting "no
       * grants" would say nobody can write to a server when the answer is unknown. */
      if (!status)
         return KB_CLIENT_GRANT_UNAVAILABLE;
      return rc == KB_CLIENT_GRANT_OK ? KB_CLIENT_GRANT_UNAVAILABLE : rc;
   }
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (rc != KB_CLIENT_GRANT_OK)
   {
      cJSON_Delete(resp);
      return rc;
   }
   const cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "grants");
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(resp);
      return KB_CLIENT_GRANT_UNAVAILABLE;
   }
   size_t written = 0;
   const cJSON *g = NULL;
   cJSON_ArrayForEach(g, arr)
   {
      if (written >= cap)
         break;
      const cJSON *sub = cJSON_GetObjectItemCaseSensitive(g, "subject");
      const cJSON *tier = cJSON_GetObjectItemCaseSensitive(g, "tier");
      /* A row without a subject and tier is not a grant. Skipping it would under-report
       * silently, so the whole listing is refused instead. */
      if (!cJSON_IsString(sub) || !cJSON_IsString(tier))
      {
         cJSON_Delete(resp);
         return KB_CLIENT_GRANT_UNAVAILABLE;
      }
      kb_client_grant_row_t *row = &out[written];
      snprintf(row->subject, sizeof(row->subject), "%s", sub->valuestring);
      snprintf(row->tier, sizeof(row->tier), "%s", tier->valuestring);
      const cJSON *by = cJSON_GetObjectItemCaseSensitive(g, "granted_by");
      if (cJSON_IsString(by))
         snprintf(row->granted_by, sizeof(row->granted_by), "%s", by->valuestring);
      const cJSON *created = cJSON_GetObjectItemCaseSensitive(g, "created_at");
      if (cJSON_IsString(created))
         snprintf(row->created_at, sizeof(row->created_at), "%s", created->valuestring);
      const cJSON *updated = cJSON_GetObjectItemCaseSensitive(g, "updated_at");
      if (cJSON_IsString(updated))
         snprintf(row->updated_at, sizeof(row->updated_at), "%s", updated->valuestring);
      /* Absent for a live grant, which is how a caller tells live from revoked without a
       * separate flag. */
      const cJSON *revoked = cJSON_GetObjectItemCaseSensitive(g, "revoked_at");
      if (cJSON_IsString(revoked))
         snprintf(row->revoked_at, sizeof(row->revoked_at), "%s", revoked->valuestring);
      ++written;
   }
   *count = written;
   if (truncated_out)
   {
      const cJSON *t = cJSON_GetObjectItemCaseSensitive(resp, "truncated");
      /* Either kb hit its own ceiling, or this buffer did. Both mean the answer is partial
       * and the caller must say so rather than present it as complete. */
      *truncated_out = cJSON_IsTrue(t) || cJSON_GetArraySize(arr) > (int)written;
   }
   cJSON_Delete(resp);
   return KB_CLIENT_GRANT_OK;
}
