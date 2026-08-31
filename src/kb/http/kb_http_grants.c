/* kb_http_grants.c — see kb_http_grants.h. */

#include "kb_http_grants.h"

#include "cJSON.h"
#include "modules/db2/c/management_intent_fields.h" /* db2_intent_canonical_actor (header-only) */
#include "modules/db2/c/db2_tenant.h"               /* db2_tenant_scope_*: sets aimee.principal */
#include "modules/db2/c/write_tier_grant.h"
#include "kb_identity.h" /* kb_identity_key: the actor's canonical identity */
#include "kb_identity_token.h"
#include "kb_reqctx.h" /* kb_reqctx_actor: the VERIFIED per-request principal */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A grant listing is bounded so a single request cannot be made to marshal an unbounded
 * number of rows. Generous for one (server, team): a deployment with more subjects than
 * this on one server has a bigger problem than pagination. */
#define GRANTS_LIST_MAX 512

static int json_error(char *out_buf, int out_cap, int status, const char *message)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "error", message);
   char *text = o ? cJSON_PrintUnformatted(o) : NULL;
   snprintf(out_buf, (size_t)out_cap, "%s", text ? text : "{\"error\":\"internal error\"}");
   free(text);
   cJSON_Delete(o);
   return status;
}

static int json_body(char *out_buf, int out_cap, int status, cJSON *o)
{
   char *text = o ? cJSON_PrintUnformatted(o) : NULL;
   if (!text || (int)strlen(text) >= out_cap)
   {
      free(text);
      cJSON_Delete(o);
      return json_error(out_buf, out_cap, 500, "response too large");
   }
   snprintf(out_buf, (size_t)out_cap, "%s", text);
   free(text);
   cJSON_Delete(o);
   return status;
}

/* db2 returns a negative tenancy code for "this needs Postgres" and -1 for everything
 * else. They must not collapse: the first is a deployment running the SQLite shim, the
 * second may be a refusal from the definer function (no admin/lead authority) or a
 * malformed argument the DB rejected. Reporting them alike would have an operator
 * debugging their credentials when the backend is simply wrong. */
static int map_db_failure(int rc, char *out_buf, int out_cap)
{
   /* Only DB2_ERR_TENANT_REQUIRES_PG means the backend is wrong. `rc < -1` swept
    * up EVERY tenancy code, so DB2_ERR_TENANT_DENIED (-104, "team not in
    * principal memberships") -- an ordinary authorization refusal -- was reported
    * as "requires the postgres backend" on a deployment already running Postgres.
    * That is exactly the confusion this function was written to prevent, in the
    * other direction: an operator whose credential simply lacks membership goes
    * looking at the database.
    *
    * Measured: a grant set against team 7 logged `tenant scope refused (rc=-104)`
    * and answered 503 "requires the postgres backend" on a Postgres kb. */
   if (rc == DB2_ERR_TENANT_REQUIRES_PG)
      return json_error(out_buf, out_cap, 503,
                        "grant administration requires the postgres backend");
   if (rc == DB2_ERR_TENANT_UNAUTHENTICATED)
      return json_error(out_buf, out_cap, 401,
                        "authentication required: the acting principal is not verifier-produced");
   if (rc == DB2_ERR_TENANT_NO_CONN || rc == DB2_ERR_TENANT_BEGIN)
      return json_error(out_buf, out_cap, 503,
                        "grant administration is temporarily unavailable: the tenant scope "
                        "could not be opened");
   if (rc == DB2_ERR_TENANT_DENIED)
      return json_error(out_buf, out_cap, 403,
                        "refused: the acting principal is not a member of that team. Grant "
                        "administration needs admin or team-lead authority IN the named team, "
                        "and the (server, team) pair must be registered");
   return json_error(out_buf, out_cap, 403,
                     "refused: grant administration requires admin or team-lead authority, "
                     "and the (server, team) pair must be registered");
}

/* Open a tenant scope for the AUTHENTICATED actor, which is what makes the definer functions
 * work at all.
 *
 * kb_write_tier_grant_set / _revoke are SECURITY DEFINER and read the acting identity from
 * aimee.principal, which only a tenant scope sets. The first version of these routes called the
 * db2 seam with NO scope open, so aimee.principal was unset, and the definer correctly refused
 * every call as "admin or team lead only" — increment 5 was wired end to end and could not
 * create a grant. Found by standing the whole stack up; nothing below this layer can see it,
 * because each layer's tests supply their own actor.
 *
 * THE ACTOR COMES FROM AUTHENTICATION, never from the request body. kb_reqctx_actor() is the
 * principal kb's verifier produced for this request — for the server's bearer that is the owner
 * principal — so a caller cannot nominate whose authority it is acting under. That is also why
 * granted_by is derived here rather than accepted: an audit trail that records a value from the
 * request can be written to order.
 *
 * Returns 0 with a scope open, or an HTTP status written into out_buf. */
static int grant_scope_begin(int64_t team_id, char *actor_key, size_t actor_cap, char *out_buf,
                             int out_cap, int *status_out)
{
   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor)
   {
      *status_out = json_error(out_buf, out_cap, 401, "authentication required");
      return -1;
   }
   if (kb_identity_key(actor, actor_key, actor_cap) != 0)
   {
      /* An authenticated principal with no derivable identity key cannot be recorded as a
       * granter, and a grant whose actor cannot be named must not be made. */
      *status_out = json_error(out_buf, out_cap, 403, "the caller has no usable identity");
      return -1;
   }
   int rc = db2_tenant_scope_begin(actor, team_id);
   if (rc != 0)
   {
      LOG_WARN("kb.grants", "tenant scope refused for team %lld (rc=%d)", (long long)team_id, rc);
      *status_out = map_db_failure(rc, out_buf, out_cap);
      return -1;
   }
   return 0;
}

/* A team id from JSON. cJSON stores every number as a double, so a bare range check
 * accepts 910001.9 and the cast silently makes it 910001 — and team_id is the
 * authorization scope, so a truncation authorizes against a team the caller did not
 * name. Anything not exactly an integer is refused rather than rounded. */
static int json_team_id(const cJSON *v, int64_t *out)
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

/* Same rule for the query-string form, which is where the read route gets it. */
static int query_team_id(const char *qs, int64_t *out);

static int tier_from_wire(const char *s, kb_identity_tier_t *out)
{
   if (!s)
      return -1;
   if (!strcmp(s, "off"))
      *out = KB_IDENTITY_TIER_OFF;
   else if (!strcmp(s, "data"))
      *out = KB_IDENTITY_TIER_DATA;
   else if (!strcmp(s, "full"))
      *out = KB_IDENTITY_TIER_FULL;
   else
      return -1;
   return 0;
}

/* Percent-decode ONE level, in place, returning 0 on success and -1 on a malformed escape.
 *
 * Required because kb_client_query_escape() escapes the subject before putting it in the query
 * string — it has to, since an oidc:<iss>:<sub> subject legitimately contains ':' and may
 * contain a literal '%3A' of its own. Reading the value back raw made every prefixed subject
 * fail the canonical-identity check: `oidc:test:alice` arrives as `oidc%3Atest%3Aalice`, which
 * contains no ':' at all and so is judged as a bare username, which forbids '%'. The effect was
 * that `show` and `list --subject` 400'd for every federated identity while a bare username
 * worked, i.e. exactly the subjects this feature exists to grant.
 *
 * Exactly ONE level, which is what round-trips the grammar: a subject containing a literal
 * '%3A' is escaped to '%253A' and must decode back to '%3A', not to ':'.
 *
 * A malformed escape is REFUSED rather than passed through: a stray '%' means the caller's
 * intent is unknown, and guessing at an identity is not something to do quietly. '+' is left
 * alone — this is a query parameter carrying an identity, not an HTML form field, and the
 * escaper emits %20 for a space, so treating '+' as a space would corrupt a subject. */
static int hexval(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

static int pct_decode_inplace(char *s)
{
   char *w = s;
   for (const char *r = s; *r; ++r)
   {
      if (*r != '%')
      {
         *w++ = *r;
         continue;
      }
      int hi = hexval(r[1]), lo = r[1] ? hexval(r[2]) : -1;
      if (hi < 0 || lo < 0)
         return -1;
      unsigned char c = (unsigned char)((hi << 4) | lo);
      /* NUL would truncate the value and C0/DEL have no business in an identity. */
      if (c < 0x20 || c == 0x7F)
         return -1;
      *w++ = (char)c;
      r += 2;
   }
   *w = '\0';
   return 0;
}

/* Read one query parameter at a KEY BOUNDARY. The generic helper elsewhere in this layer
 * does strstr(qs, "key="), which matches inside "?my_team_id=" — untidy for a filter and
 * wrong for anything that selects an authorization scope, which team_id does.
 *
 * The value is percent-decoded before it is returned, so every caller sees the value the
 * client sent rather than its wire encoding. */
static int query_param(const char *qs, const char *key, char *out, size_t cap)
{
   if (cap)
      out[0] = '\0';
   if (!qs || !key || !cap)
      return 0;
   size_t keylen = strlen(key);
   for (const char *p = qs; *p;)
   {
      const char *amp = strchr(p, '&');
      const char *end = amp ? amp : p + strlen(p);
      const char *eq = memchr(p, '=', (size_t)(end - p));
      if (eq && (size_t)(eq - p) == keylen && !memcmp(p, key, keylen))
      {
         size_t n = (size_t)(end - eq - 1);
         if (n >= cap)
            return -1; /* refused, not truncated */
         memcpy(out, eq + 1, n);
         out[n] = '\0';
         /* Decoding only ever shortens, so it cannot overflow the cap checked above. */
         if (pct_decode_inplace(out) != 0)
         {
            if (cap)
               out[0] = '\0';
            return -1;
         }
         return 1;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   return 0;
}

static int query_team_id(const char *qs, int64_t *out)
{
   *out = 0;
   char raw[32] = "";
   if (query_param(qs, "team_id", raw, sizeof(raw)) != 1 || !raw[0])
      return -1;
   char *tail = NULL;
   long long v = strtoll(raw, &tail, 10);
   /* Only digits, and nothing after them: "910001abc" and "910001.9" are refused rather
    * than silently becoming 910001. */
   if (!tail || *tail || v < 1 || v > 9007199254740991LL)
      return -1;
   *out = (int64_t)v;
   return 0;
}

/* The subject grammar the grant table CHECKs, validated before the round trip so a
 * malformed subject is a clean 400 rather than a constraint violation surfaced as a
 * generic refusal.
 *
 * db2_intent_canonical_actor takes a FIXED-SIZE ZERO-PADDED RECORD, not an arbitrary
 * pointer — it verifies the unused tail is zero, so handing it a bare string reads past
 * the end of that string. Hence the copy. Getting this wrong reads out of bounds and
 * yields a confident "invalid" for a perfectly good subject. */
static int subject_valid(const char *s)
{
   char record[DB2_INTENT_ACTOR_MAX + 1];
   if (!s || !s[0] || strlen(s) > DB2_INTENT_ACTOR_MAX)
      return 0;
   memset(record, 0, sizeof(record));
   memcpy(record, s, strlen(s));
   return db2_intent_canonical_actor(record, sizeof(record));
}

static int server_id_valid(const char *s)
{
   if (!s || !s[0] || strlen(s) > 127)
      return 0;
   for (size_t i = 0; s[i]; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      int alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

/* POST /v1/write-tier-grants/set */
static int post_set(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const cJSON *jsrv = req ? cJSON_GetObjectItemCaseSensitive(req, "server_id") : NULL;
   const cJSON *jteam = req ? cJSON_GetObjectItemCaseSensitive(req, "team_id") : NULL;
   const cJSON *jsub = req ? cJSON_GetObjectItemCaseSensitive(req, "subject") : NULL;
   const cJSON *jtier = req ? cJSON_GetObjectItemCaseSensitive(req, "tier") : NULL;
   const cJSON *jby = req ? cJSON_GetObjectItemCaseSensitive(req, "granted_by") : NULL;

   int64_t team_id = 0;
   kb_identity_tier_t tier = KB_IDENTITY_TIER_OFF;
   char server_id[128] = "", subject[578] = "", granted_by[578] = "";
   int ok = cJSON_IsString(jsrv) && server_id_valid(jsrv->valuestring) &&
            json_team_id(jteam, &team_id) == 0 && cJSON_IsString(jsub) &&
            subject_valid(jsub->valuestring) && cJSON_IsString(jtier) &&
            tier_from_wire(jtier->valuestring, &tier) == 0 && cJSON_IsString(jby) &&
            jby->valuestring && jby->valuestring[0] &&
            strlen(jby->valuestring) < sizeof(granted_by);
   if (ok)
   {
      snprintf(server_id, sizeof(server_id), "%s", jsrv->valuestring);
      snprintf(subject, sizeof(subject), "%s", jsub->valuestring);
      snprintf(granted_by, sizeof(granted_by), "%s", jby->valuestring);
   }
   cJSON_Delete(req);
   if (!ok)
      return json_error(out_buf, out_cap, 400,
                        "server_id, an integer team_id, a canonical subject, "
                        "tier (off|data|full) and granted_by are required");

   /* The REPORTING variant, not the plain setter: `changed` tells an operator script that
    * somebody's access just grew, and it can only be trusted when the prior state was
    * observed under the same lock as the write. */
   char actor_key[578] = "";
   int scope_status = 0;
   if (grant_scope_begin(team_id, actor_key, sizeof(actor_key), out_buf, out_cap, &scope_status) !=
       0)
      return scope_status;

   /* granted_by is the AUTHENTICATED actor, not the `granted_by` the request carried. The wire
    * field is ignored precisely because an audit trail recording a caller-supplied name can be
    * written to order. */
   db2_write_tier_grant_report_t report;
   int rc =
       db2_write_tier_grant_set_reporting(server_id, team_id, subject, tier, actor_key, &report);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      LOG_WARN("kb.grants", "write-tier grant set refused for %s on %s (rc=%d)", subject, server_id,
               rc);
      return map_db_failure(rc, out_buf, out_cap);
   }

   if (db2_tenant_scope_commit() != 0)
      /* The grant may or may not exist. Unavailable rather than success: a caller told
       * "granted" would not retry, and one told nothing would. */
      return json_error(out_buf, out_cap, 503,
                        "the grant may not have been committed; re-run to confirm");
   LOG_INFO("kb.grants", "write-tier grant %s for %s on %s team %lld -> %s (by %s)",
            report.changed ? "changed" : "unchanged", subject, server_id, (long long)team_id,
            kb_identity_tier_str(tier), actor_key);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddBoolToObject(o, "changed", report.changed);
   cJSON_AddBoolToObject(o, "was_revoked", report.was_revoked);
   /* Absent rather than null-or-empty when the grant did not exist: "created" and
    * "changed from off" are different facts and must not render alike. */
   if (report.had_previous)
      cJSON_AddStringToObject(o, "previous_tier", kb_identity_tier_str(report.previous_tier));
   cJSON_AddBoolToObject(o, "is_member", report.is_member);
   return json_body(out_buf, out_cap, 200, o);
}

/* POST /v1/write-tier-grants/revoke */
static int post_revoke(const char *body, char *out_buf, int out_cap)
{
   cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
   const cJSON *jsrv = req ? cJSON_GetObjectItemCaseSensitive(req, "server_id") : NULL;
   const cJSON *jteam = req ? cJSON_GetObjectItemCaseSensitive(req, "team_id") : NULL;
   const cJSON *jsub = req ? cJSON_GetObjectItemCaseSensitive(req, "subject") : NULL;
   int64_t team_id = 0;
   char server_id[128] = "", subject[578] = "";
   int ok = cJSON_IsString(jsrv) && server_id_valid(jsrv->valuestring) &&
            json_team_id(jteam, &team_id) == 0 && cJSON_IsString(jsub) &&
            subject_valid(jsub->valuestring);
   if (ok)
   {
      snprintf(server_id, sizeof(server_id), "%s", jsrv->valuestring);
      snprintf(subject, sizeof(subject), "%s", jsub->valuestring);
   }
   cJSON_Delete(req);
   if (!ok)
      return json_error(out_buf, out_cap, 400,
                        "server_id, an integer team_id and a canonical subject are required");

   /* Whether a live grant existed is read BEFORE the revoke, so the caller can be told
    * "there was nothing to revoke" — a likely typo'd subject — apart from "it was already
    * revoked", where the operator's intent is already satisfied.
    *
    * AN EXACT LOOKUP, not a scan of a listing. The first version fetched the general listing
    * (capped at GRANTS_LIST_MAX) and searched it in C, so a subject sorting beyond the cap
    * was reported found:false while being successfully revoked — telling an operator nothing
    * was there when something was. A review caught it. A lookup asks about exactly this
    * subject and cannot be crowded out by others.
    *
    * The revoke itself is idempotent, so a race between this read and the write costs at
    * worst a misreported `found`, never a wrong outcome. */
   char actor_key[578] = "";
   int scope_status = 0;
   if (grant_scope_begin(team_id, actor_key, sizeof(actor_key), out_buf, out_cap, &scope_status) !=
       0)
      return scope_status;

   kb_identity_tier_t existing = KB_IDENTITY_TIER_OFF;
   int lookup = db2_write_tier_grant_lookup(server_id, team_id, subject, &existing);
   if (lookup < 0)
   {
      db2_tenant_scope_rollback();
      /* A failed lookup must not be reported as "no grant existed": that is an authoritative
       * claim this call is in no position to make. */
      LOG_WARN("kb.grants", "could not determine whether a grant existed for %s on %s (rc=%d)",
               subject, server_id, lookup);
      return map_db_failure(lookup, out_buf, out_cap);
   }
   int found = (lookup == DB2_WRITE_TIER_GRANT_FOUND);

   int rc = db2_write_tier_grant_revoke(server_id, team_id, subject);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      LOG_WARN("kb.grants", "write-tier grant revoke refused for %s on %s (rc=%d)", subject,
               server_id, rc);
      return map_db_failure(rc, out_buf, out_cap);
   }
   if (db2_tenant_scope_commit() != 0)
      return json_error(out_buf, out_cap, 503,
                        "the revocation may not have been committed; re-run to confirm");
   LOG_INFO("kb.grants", "write-tier grant revoked for %s on %s team %lld (existed=%d, by %s)",
            subject, server_id, (long long)team_id, found, actor_key);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddBoolToObject(o, "found", found);
   return json_body(out_buf, out_cap, 200, o);
}

/* GET /v1/write-tier-grants?server_id&team_id[&include_revoked=1][&subject] */
static int get_list(const char *query_string, char *out_buf, int out_cap)
{
   char server_id[128] = "", subject[578] = "", flag[8] = "";
   int64_t team_id = 0;
   if (query_param(query_string, "server_id", server_id, sizeof(server_id)) != 1 ||
       !server_id_valid(server_id) || query_team_id(query_string, &team_id) != 0)
      return json_error(out_buf, out_cap, 400, "server_id and an integer team_id are required");
   /* A malformed or oversized subject must be REFUSED, never demoted to "no filter": treating
    * it as absent would answer a request for one subject by listing every grant on the server,
    * which is both a wider disclosure than was asked for and a wrong answer. */
   int sub_rc = query_param(query_string, "subject", subject, sizeof(subject));
   if (sub_rc < 0)
      return json_error(out_buf, out_cap, 400, "subject is not a canonical identity");
   int have_subject = sub_rc == 1;
   if (have_subject && !subject_valid(subject))
      return json_error(out_buf, out_cap, 400, "subject is not a canonical identity");
   /* Anything other than an explicit 1/true widens nothing — an unrecognised value must
    * not be read as "yes" on a parameter that reveals revoked history. */
   int include_revoked = 0;
   if (query_param(query_string, "include_revoked", flag, sizeof(flag)) == 1)
      include_revoked = (!strcmp(flag, "1") || !strcmp(flag, "true"));

   char actor_key[578] = "";
   int scope_status = 0;
   if (grant_scope_begin(team_id, actor_key, sizeof(actor_key), out_buf, out_cap, &scope_status) !=
       0)
      return scope_status;

   db2_write_tier_grant_row_t rows[GRANTS_LIST_MAX];
   size_t count = 0;
   /* The subject filter goes DOWN to the query, not applied to the rows that come back:
    * filtering after a capped fetch hides a subject sorting beyond the cap, so `show` would
    * answer "no grant" for a subject that has one. */
   int rc =
       db2_write_tier_grant_list_ex(server_id, team_id, include_revoked,
                                    have_subject ? subject : NULL, rows, GRANTS_LIST_MAX, &count);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return map_db_failure(rc, out_buf, out_cap);
   }
   /* A read, so a failed commit is not ambiguous the way a write's is. */
   if (db2_tenant_scope_commit() != 0)
      return json_error(out_buf, out_cap, 503, "could not read the grants");

   cJSON *o = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(o, "grants");
   if (!arr)
   {
      cJSON_Delete(o);
      return json_error(out_buf, out_cap, 500, "internal error");
   }
   for (size_t i = 0; i < count; ++i)
   {
      /* `show` is this listing filtered to one subject, done here rather than as its own
       * route so the row shape has exactly one definition. */
      cJSON *g = cJSON_CreateObject();
      cJSON_AddStringToObject(g, "subject", rows[i].subject);
      cJSON_AddStringToObject(g, "tier", kb_identity_tier_str(rows[i].tier));
      cJSON_AddStringToObject(g, "granted_by", rows[i].granted_by);
      cJSON_AddStringToObject(g, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(g, "updated_at", rows[i].updated_at);
      /* Present only when actually revoked, so a live grant carries no misleading key. */
      if (rows[i].revoked_at[0])
         cJSON_AddStringToObject(g, "revoked_at", rows[i].revoked_at);
      cJSON_AddItemToArray(arr, g);
   }
   /* Reported so a caller can tell a full page from a complete answer rather than
    * inferring it from the array length. */
   cJSON_AddBoolToObject(o, "truncated", count == GRANTS_LIST_MAX);
   return json_body(out_buf, out_cap, 200, o);
}

int kb_http_grants_route(const char *method, const char *path, const char *query_string,
                         const char *body, char *out_buf, int out_cap)
{
   if (!method || !path || !out_buf || out_cap <= 0)
      return -1;
   if (!strcmp(path, "/v1/write-tier-grants/set"))
      return strcmp(method, "POST") ? json_error(out_buf, out_cap, 405, "method not allowed")
                                    : post_set(body, out_buf, out_cap);
   if (!strcmp(path, "/v1/write-tier-grants/revoke"))
      return strcmp(method, "POST") ? json_error(out_buf, out_cap, 405, "method not allowed")
                                    : post_revoke(body, out_buf, out_cap);
   if (!strcmp(path, "/v1/write-tier-grants"))
      return strcmp(method, "GET") ? json_error(out_buf, out_cap, 405, "method not allowed")
                                   : get_list(query_string, out_buf, out_cap);
   return -1;
}
