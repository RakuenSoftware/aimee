/* test_kb_http_grants.c — the kb write-tier grant routes.
 *
 * These routes administer who may write to a remote server, so what they REFUSE matters
 * more than what they return. The db2 seam is stubbed: it needs Postgres, and its own
 * behaviour is covered by the P1 RLS gate. What is tested here is the routing and
 * validation layer — the part that decides whether a request reaches the seam at all, and
 * with what arguments.
 *
 * Pinned in particular:
 *   - team_id is an authorization scope, so a fractional or unrepresentable value must be
 *     refused rather than silently truncated into a DIFFERENT team
 *   - query parameters are matched at a KEY BOUNDARY, so "?my_team_id=" does not supply
 *     team_id
 *   - include_revoked widens only on an explicit 1/true; an unrecognised value must not be
 *     read as "yes" on a parameter that reveals revoked history
 *   - a "needs Postgres" failure and a refusal are DIFFERENT answers, so an operator is
 *     not sent to debug credentials when the backend is wrong
 *   - previous_tier is ABSENT when the grant did not exist, because "created" and "changed
 *     from off" are different facts
 */
#include "kb_http_grants.h"

#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h" /* the real tenancy codes this maps from */
#include "modules/db2/c/write_tier_grant.h"
#include "kb_identity.h"
#include "kb_reqctx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── The db2 seam, stubbed ─────────────────────────────────────────────────── */

static int stub_rc; /* what the seam returns */
static int stub_set_calls;
static int stub_revoke_calls;
static int stub_list_calls;
static int stub_last_include_revoked;
static int stub_last_had_subject;
static int stub_lookup_calls;
static int stub_lookup_rc;
static int64_t stub_last_team;
static char stub_last_subject[600];
static char stub_last_server[200];
static char stub_last_granted_by[600];
static kb_identity_tier_t stub_last_tier;
static db2_write_tier_grant_report_t stub_report;
static size_t stub_row_count;
static db2_write_tier_grant_row_t stub_rows[4];

int db2_write_tier_grant_set_reporting(const char *server_id, int64_t team_id, const char *subject,
                                       kb_identity_tier_t tier, const char *granted_by,
                                       db2_write_tier_grant_report_t *out)
{
   stub_set_calls++;
   assert(server_id && subject && granted_by && out);
   snprintf(stub_last_server, sizeof(stub_last_server), "%s", server_id);
   snprintf(stub_last_subject, sizeof(stub_last_subject), "%s", subject);
   snprintf(stub_last_granted_by, sizeof(stub_last_granted_by), "%s", granted_by ? granted_by : "");
   stub_last_team = team_id;
   stub_last_tier = tier;
   memset(out, 0, sizeof(*out));
   if (stub_rc != 0)
      return stub_rc;
   *out = stub_report;
   return 0;
}

int db2_write_tier_grant_revoke(const char *server_id, int64_t team_id, const char *subject)
{
   stub_revoke_calls++;
   snprintf(stub_last_server, sizeof(stub_last_server), "%s", server_id ? server_id : "");
   snprintf(stub_last_subject, sizeof(stub_last_subject), "%s", subject ? subject : "");
   stub_last_team = team_id;
   return stub_rc;
}

int db2_write_tier_grant_list_ex(const char *server_id, int64_t team_id, int include_revoked,
                                 const char *subject, db2_write_tier_grant_row_t *out, size_t cap,
                                 size_t *count)
{
   stub_list_calls++;
   stub_last_include_revoked = include_revoked;
   /* Recorded so a test can prove the filter went DOWN to the query rather than being applied
    * to whatever rows came back. */
   stub_last_had_subject = (subject && subject[0]) ? 1 : 0;
   /* The VALUE too, not just its presence: the subject arrives percent-escaped on the wire, so
    * a test has to be able to prove which principal was actually asked about. */
   snprintf(stub_last_subject, sizeof(stub_last_subject), "%s", subject ? subject : "");
   stub_last_team = team_id;
   snprintf(stub_last_server, sizeof(stub_last_server), "%s", server_id ? server_id : "");
   if (count)
      *count = 0;
   if (stub_rc != 0)
      return stub_rc;
   size_t n = stub_row_count < cap ? stub_row_count : cap;
   for (size_t i = 0; i < n; ++i)
      out[i] = stub_rows[i];
   if (count)
      *count = n;
   return 0;
}

/* Linked but unused by these routes; present so the object set resolves. */
int db2_write_tier_grant_set(const char *server_id, int64_t team_id, const char *subject,
                             kb_identity_tier_t tier, const char *granted_by)
{
   (void)server_id;
   (void)team_id;
   (void)subject;
   (void)tier;
   (void)granted_by;
   return -1;
}

int db2_write_tier_grant_list(const char *server_id, int64_t team_id,
                              db2_write_tier_grant_row_t *out, size_t cap, size_t *count)
{
   return db2_write_tier_grant_list_ex(server_id, team_id, 0, NULL, out, cap, count);
}

int db2_write_tier_grant_lookup(const char *server_id, int64_t team_id, const char *subject,
                                kb_identity_tier_t *out)
{
   stub_lookup_calls++;
   (void)server_id;
   (void)team_id;
   (void)subject;
   if (out)
      *out = KB_IDENTITY_TIER_DATA;
   return stub_lookup_rc;
}

/* The routes now open a tenant scope for the AUTHENTICATED actor, because the definer functions
 * read aimee.principal and a call with no scope is refused as "admin or team lead only" — which
 * is how increment 5 shipped wired-but-unusable until a live run found it. These stubs let the
 * routing tests supply an actor and observe that a scope was opened and closed. */
static int stub_scope_begins;
static int stub_scope_commits;
static int stub_scope_rollbacks;
static int stub_scope_rc;
static kb_principal_t stub_actor;
static int stub_have_actor;

const kb_principal_t *kb_reqctx_actor(void)
{
   return stub_have_actor ? &stub_actor : NULL;
}

int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team)
{
   stub_scope_begins++;
   (void)p;
   (void)team;
   return stub_scope_rc;
}

int db2_tenant_scope_commit(void)
{
   stub_scope_commits++;
   return 0;
}

void db2_tenant_scope_rollback(void)
{
   stub_scope_rollbacks++;
}

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void reset(void)
{
   stub_rc = 0;
   stub_set_calls = stub_revoke_calls = stub_list_calls = 0;
   stub_last_include_revoked = -1;
   stub_last_had_subject = -1;
   stub_lookup_calls = 0;
   stub_lookup_rc = DB2_WRITE_TIER_GRANT_NONE;
   stub_scope_begins = stub_scope_commits = stub_scope_rollbacks = 0;
   stub_scope_rc = 0;
   /* An owner principal, which is what kb's verifier produces for the server's bearer. */
   memset(&stub_actor, 0, sizeof(stub_actor));
   stub_actor.kind = KB_PRIN_OWNER;
   stub_actor.authenticated = 1;
   stub_have_actor = 1;
   stub_last_team = 0;
   stub_last_subject[0] = stub_last_server[0] = '\0';
   stub_row_count = 0;
   memset(&stub_report, 0, sizeof(stub_report));
   memset(stub_rows, 0, sizeof(stub_rows));
}

static int route(const char *method, const char *path, const char *qs, const char *body, char *out,
                 int cap)
{
   return kb_http_grants_route(method, path, qs, body, out, cap);
}

static void row(size_t i, const char *subject, kb_identity_tier_t tier, const char *revoked_at)
{
   snprintf(stub_rows[i].subject, sizeof(stub_rows[i].subject), "%s", subject);
   stub_rows[i].tier = tier;
   snprintf(stub_rows[i].granted_by, sizeof(stub_rows[i].granted_by), "%s", "owner");
   snprintf(stub_rows[i].created_at, sizeof(stub_rows[i].created_at), "%s", "2026-01-01");
   snprintf(stub_rows[i].updated_at, sizeof(stub_rows[i].updated_at), "%s", "2026-01-02");
   snprintf(stub_rows[i].revoked_at, sizeof(stub_rows[i].revoked_at), "%s", revoked_at);
   if (i + 1 > stub_row_count)
      stub_row_count = i + 1;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_not_our_routes(void)
{
   char out[4096] = "";
   /* -1 means "not mine", so an unrelated path reaches the rest of the router. A satellite
    * that claimed a path it does not own would silently shadow it. */
   assert(route("GET", "/v1/health", NULL, NULL, out, sizeof(out)) == -1);
   assert(route("GET", "/v1/write-tier-grant", NULL, NULL, out, sizeof(out)) == -1);
   assert(route("GET", "/v1/write-tier-grants/extra", NULL, NULL, out, sizeof(out)) == -1);
   assert(route("POST", "/v1/write-tier-grants/set/extra", NULL, NULL, out, sizeof(out)) == -1);
   assert(route(NULL, "/v1/write-tier-grants", NULL, NULL, out, sizeof(out)) == -1);
   assert(route("GET", NULL, NULL, NULL, out, sizeof(out)) == -1);
}

static void test_methods(void)
{
   char out[4096] = "";
   reset();
   assert(route("GET", "/v1/write-tier-grants/set", NULL, NULL, out, sizeof(out)) == 405);
   assert(route("PUT", "/v1/write-tier-grants/revoke", NULL, NULL, out, sizeof(out)) == 405);
   assert(route("POST", "/v1/write-tier-grants", NULL, NULL, out, sizeof(out)) == 405);
   /* A wrong method must not reach the database. */
   assert(stub_set_calls == 0 && stub_revoke_calls == 0 && stub_list_calls == 0);
}

static void test_set(void)
{
   char out[4096] = "";
   const char *good = "{\"server_id\":\"srv1\",\"team_id\":910001,"
                      "\"subject\":\"oidc:iss:alice\",\"tier\":\"data\",\"granted_by\":\"owner\"}";

   /* Created: changed, and NO previous_tier key, because "created" and "changed from off"
    * are different facts and must not render alike. */
   reset();
   stub_report.changed = 1;
   stub_report.is_member = 1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 200);
   assert(strstr(out, "\"changed\":true"));
   assert(strstr(out, "\"was_revoked\":false"));
   assert(strstr(out, "\"is_member\":true"));
   assert(!strstr(out, "previous_tier"));
   assert(stub_set_calls == 1);
   /* The arguments reached the seam unchanged. */
   assert(!strcmp(stub_last_server, "srv1") && stub_last_team == 910001);
   assert(!strcmp(stub_last_subject, "oidc:iss:alice"));
   assert(stub_last_tier == KB_IDENTITY_TIER_DATA);

   /* Re-tiered: previous_tier IS present, which is the signal a script uses to notice that
    * somebody's access changed. */
   reset();
   stub_report.changed = 1;
   stub_report.had_previous = 1;
   stub_report.previous_tier = KB_IDENTITY_TIER_DATA;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 200);
   assert(strstr(out, "\"previous_tier\":\"data\""));

   /* was_revoked surfaces a re-grant of a revoked subject. */
   reset();
   stub_report.changed = 1;
   stub_report.was_revoked = 1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 200);
   assert(strstr(out, "\"was_revoked\":true"));

   /* A non-member is REPORTED, not refused: memberships and grants are provisioned in
    * either order, so the operator gets a warning rather than a failure. */
   reset();
   stub_report.changed = 1;
   stub_report.is_member = 0;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 200);
   assert(strstr(out, "\"is_member\":false"));
}

static void test_set_rejects(void)
{
   char out[4096] = "";
   /* Every one of these must be a 400 and must NOT reach the database. */
   const char *bad[] = {
       /* missing fields */
       "{\"team_id\":1,\"subject\":\"owner\",\"tier\":\"data\",\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"subject\":\"owner\",\"tier\":\"data\",\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":1,\"tier\":\"data\",\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\",\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"data\"}",
       /* TEAM ID PRECISION. cJSON stores numbers as doubles, so 910001.9 would pass a bare
        * range check and become 910001 — authorizing against a team nobody named. */
       "{\"server_id\":\"s\",\"team_id\":910001.9,\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":0,\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":-1,\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":9007199254740993,\"subject\":\"owner\","
       "\"tier\":\"data\",\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":\"910001\",\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       /* tier outside the three */
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"root\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"\","
       "\"granted_by\":\"o\"}",
       /* subjects outside the canonical grammar the grant table CHECKs */
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"has space\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"-leading\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       /* server ids outside the identifier grammar */
       "{\"server_id\":\"has space\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"-bad\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "{\"server_id\":\"\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"data\","
       "\"granted_by\":\"o\"}",
       "not json",
       "",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      reset();
      int st = route("POST", "/v1/write-tier-grants/set", NULL, bad[i], out, sizeof(out));
      if (st != 400 || stub_set_calls != 0)
      {
         fprintf(stderr, "set accepted or forwarded a bad body (status %d, calls %d): %s\n", st,
                 stub_set_calls, bad[i]);
         assert(0);
      }
   }
   reset();
   assert(route("POST", "/v1/write-tier-grants/set", NULL, NULL, out, sizeof(out)) == 400);
   assert(stub_set_calls == 0);

   /* A valid team id at the precision BOUNDARY must still be accepted, so the guard does
    * not over-reach: 2^53 - 1 is exactly representable. */
   reset();
   stub_report.changed = 1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL,
                "{\"server_id\":\"s\",\"team_id\":9007199254740991,\"subject\":\"owner\","
                "\"tier\":\"full\",\"granted_by\":\"o\"}",
                out, sizeof(out)) == 200);
   assert(stub_last_team == 9007199254740991LL);
}

/* THE TENANT SCOPE IS THE THING THAT MAKES THESE ROUTES WORK AT ALL.
 *
 * kb_write_tier_grant_set / _revoke are SECURITY DEFINER and read the acting identity from
 * aimee.principal, which only a tenant scope sets. The first version of these routes opened no
 * scope, so every call was refused as "admin or team lead only" — increment 5 was wired end to
 * end and could not create a grant. A live run found it; no layer-local test could, because each
 * layer's tests supply their own actor. */
static void test_actor_scope(void)
{
   char out[4096] = "";
   const char *good = "{\"server_id\":\"srv1\",\"team_id\":910001,\"subject\":\"owner\","
                      "\"tier\":\"data\",\"granted_by\":\"owner\"}";

   /* A scope is opened and COMMITTED on the success path. */
   reset();
   stub_report.changed = 1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 200);
   assert(stub_scope_begins == 1 && stub_scope_commits == 1 && stub_scope_rollbacks == 0);

   /* NO AUTHENTICATED ACTOR: refused 401, and the database is never touched. A grant whose actor
    * cannot be named must not be made. */
   reset();
   stub_have_actor = 0;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 401);
   assert(stub_set_calls == 0 && stub_scope_begins == 0);
   reset();
   stub_have_actor = 0;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL,
                "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\"}", out,
                sizeof(out)) == 401);
   assert(stub_revoke_calls == 0);
   reset();
   stub_have_actor = 0;
   assert(route("GET", "/v1/write-tier-grants", "server_id=s&team_id=1", NULL, out, sizeof(out)) ==
          401);
   assert(stub_list_calls == 0);

   /* A scope that cannot be opened refuses before the write, and rolls nothing back because
    * nothing began. */
   reset();
   stub_scope_rc = -1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 403);
   assert(stub_set_calls == 0);

   /* A FAILING db call ROLLS BACK rather than leaving a scope open on the connection. */
   reset();
   stub_rc = -1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 403);
   assert(stub_scope_begins == 1 && stub_scope_rollbacks == 1 && stub_scope_commits == 0);
   reset();
   stub_lookup_rc = -1;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL,
                "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\"}", out,
                sizeof(out)) == 403);
   assert(stub_scope_rollbacks == 1 && stub_scope_commits == 0);

   /* THE GRANTER IS THE AUTHENTICATED ACTOR, not the request's granted_by. A body that names
    * somebody else must not change who the audit trail records. */
   reset();
   stub_report.changed = 1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL,
                "{\"server_id\":\"srv1\",\"team_id\":910001,\"subject\":\"alice\","
                "\"tier\":\"data\",\"granted_by\":\"mallory\"}",
                out, sizeof(out)) == 200);
   assert(!strcmp(stub_last_granted_by, "owner"));
}

static void test_db_failures(void)
{
   char out[4096] = "";
   const char *good = "{\"server_id\":\"srv1\",\"team_id\":910001,\"subject\":\"owner\","
                      "\"tier\":\"data\",\"granted_by\":\"owner\"}";

   /* A REFUSAL from the definer function (no admin/lead authority, or an unregistered
    * (server, team)) is a 403. */
   reset();
   stub_rc = -1;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 403);
   assert(strstr(out, "admin or team-lead authority"));

   /* REQUIRES_PG means the backend is not Postgres — a deployment fault, not an
    * authorization one. Collapsing the two would send an operator to debug credentials
    * when the answer is that RLS cannot be enforced at all on this backend. */
   reset();
   stub_rc = DB2_ERR_TENANT_REQUIRES_PG;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 503);
   assert(strstr(out, "postgres backend"));

   /* DENIED is the OPPOSITE fault and must not report as the one above. The rule was
    * `rc < -1`, which swept up every tenancy code, so a caller who simply is not a member
    * of the team was told the backend was wrong — on a deployment already running
    * Postgres. Measured live: `tenant scope refused (rc=-104)` answering 503 "requires the
    * postgres backend". A fixed `-42` used to stand in here, which is not a code the
    * tenancy layer can return, so the test agreed with the collapse instead of catching
    * it. The real codes are used now. */
   reset();
   stub_rc = DB2_ERR_TENANT_DENIED;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 403);
   assert(strstr(out, "not a member of that team"));
   assert(!strstr(out, "postgres backend"));

   /* Unauthenticated and scope-open failures are likewise their own answers. */
   reset();
   stub_rc = DB2_ERR_TENANT_UNAUTHENTICATED;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 401);
   reset();
   stub_rc = DB2_ERR_TENANT_NO_CONN;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 503);
   assert(!strstr(out, "postgres backend"));

   reset();
   stub_rc = DB2_ERR_TENANT_REQUIRES_PG;
   assert(route("GET", "/v1/write-tier-grants", "server_id=s&team_id=1", NULL, out, sizeof(out)) ==
          503);
   reset();
   stub_rc = -1;
   assert(route("GET", "/v1/write-tier-grants", "server_id=s&team_id=1", NULL, out, sizeof(out)) ==
          403);
}

static void test_revoke(void)
{
   char out[4096] = "";
   const char *good = "{\"server_id\":\"srv1\",\"team_id\":910001,\"subject\":\"oidc:iss:alice\"}";

   /* `found` COMES FROM AN EXACT LOOKUP, not from scanning a listing.
    *
    * The first version searched the general (capped) listing, so a subject sorting beyond the
    * cap was reported found:false while being successfully revoked — telling an operator
    * nothing was there when something was. A review caught it. The assertions below are that
    * the lookup is consulted and the listing is NOT. */
   reset();
   stub_lookup_rc = DB2_WRITE_TIER_GRANT_FOUND;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL, good, out, sizeof(out)) == 200);
   assert(strstr(out, "\"found\":true"));
   assert(stub_revoke_calls == 1);
   assert(stub_lookup_calls == 1);
   assert(stub_list_calls == 0); /* a listing must not be involved at all */

   /* Nothing to revoke: found = false. Reported rather than treated as success, because a
    * subject that was never granted is usually a typo, and silently succeeding would let an
    * operator believe they closed access they never held. */
   reset();
   stub_lookup_rc = DB2_WRITE_TIER_GRANT_NONE;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL, good, out, sizeof(out)) == 200);
   assert(strstr(out, "\"found\":false"));
   /* The revoke still ran: it is idempotent, and refusing here would make a retry after a
    * timeout fail. */
   assert(stub_revoke_calls == 1);
   assert(stub_list_calls == 0);

   /* A FAILED lookup must not be reported as "no grant existed" — that is an authoritative
    * claim this call is in no position to make. */
   reset();
   stub_lookup_rc = -1;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL, good, out, sizeof(out)) == 403);
   assert(stub_revoke_calls == 0); /* and nothing was revoked on a broken read */
   reset();
   stub_lookup_rc = DB2_ERR_TENANT_REQUIRES_PG;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL, good, out, sizeof(out)) == 503);
   assert(stub_revoke_calls == 0);
   /* A membership refusal on the read is a 403 here too, and still revokes nothing. */
   reset();
   stub_lookup_rc = DB2_ERR_TENANT_DENIED;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL, good, out, sizeof(out)) == 403);
   assert(stub_revoke_calls == 0);

   /* Malformed bodies never reach the database. */
   const char *bad[] = {"{\"team_id\":1,\"subject\":\"owner\"}",
                        "{\"server_id\":\"s\",\"subject\":\"owner\"}",
                        "{\"server_id\":\"s\",\"team_id\":1}",
                        "{\"server_id\":\"s\",\"team_id\":1.5,\"subject\":\"owner\"}",
                        "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"has space\"}",
                        ""};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      reset();
      assert(route("POST", "/v1/write-tier-grants/revoke", NULL, bad[i], out, sizeof(out)) == 400);
      assert(stub_revoke_calls == 0);
   }
}

static void test_list(void)
{
   char out[8192] = "";

   /* The default listing does not widen. */
   reset();
   row(0, "oidc:iss:alice", KB_IDENTITY_TIER_DATA, "");
   assert(route("GET", "/v1/write-tier-grants", "server_id=srv1&team_id=910001", NULL, out,
                sizeof(out)) == 200);
   assert(stub_last_include_revoked == 0);
   assert(strstr(out, "\"subject\":\"oidc:iss:alice\"") && strstr(out, "\"tier\":\"data\""));
   /* A live grant carries no revoked_at key, so its absence is meaningful. */
   assert(!strstr(out, "revoked_at"));
   assert(strstr(out, "\"truncated\":false"));
   assert(!strcmp(stub_last_server, "srv1") && stub_last_team == 910001);

   /* include_revoked WIDENS, and only on an explicit 1/true. Anything else must not be
    * read as "yes" on a parameter that reveals revoked history. */
   const char *yes[] = {"1", "true"};
   for (size_t i = 0; i < 2; ++i)
   {
      char qs[128];
      snprintf(qs, sizeof(qs), "server_id=srv1&team_id=910001&include_revoked=%s", yes[i]);
      reset();
      row(0, "oidc:iss:alice", KB_IDENTITY_TIER_DATA, "2026-02-02");
      assert(route("GET", "/v1/write-tier-grants", qs, NULL, out, sizeof(out)) == 200);
      assert(stub_last_include_revoked == 1);
      assert(strstr(out, "\"revoked_at\":\"2026-02-02\""));
   }
   const char *no[] = {"0", "false", "yes", "TRUE", "", "2"};
   for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); ++i)
   {
      char qs[128];
      snprintf(qs, sizeof(qs), "server_id=srv1&team_id=910001&include_revoked=%s", no[i]);
      reset();
      assert(route("GET", "/v1/write-tier-grants", qs, NULL, out, sizeof(out)) == 200);
      if (stub_last_include_revoked != 0)
      {
         fprintf(stderr, "include_revoked=%s widened the listing\n", no[i]);
         assert(0);
      }
   }

   /* `show` is this listing filtered to one subject — AND THE FILTER GOES DOWN TO THE QUERY.
    *
    * The first version fetched the general listing (capped at GRANTS_LIST_MAX) and filtered it
    * in C. A review pointed out the consequence: a subject sorting beyond the cap is invisible,
    * so `show` answers "no grant" for a subject that has one, purely because others sort ahead
    * of it. The assertion is therefore about WHERE the filter was applied, not just the output.
    *
    * The stub returns a row whose subject differs from the request, and the route must return
    * it VERBATIM — because selecting rows is now the database's job. A route still filtering in
    * C would drop it and this would fail. */
   reset();
   row(0, "oidc:iss:alice", KB_IDENTITY_TIER_DATA, "");
   assert(route("GET", "/v1/write-tier-grants",
                "server_id=srv1&team_id=910001&subject=oidc:iss:bob", NULL, out,
                sizeof(out)) == 200);
   assert(stub_last_had_subject == 1);
   assert(strstr(out, "oidc:iss:alice"));
   /* And a listing with no subject passes none down. */
   reset();
   row(0, "oidc:iss:alice", KB_IDENTITY_TIER_DATA, "");
   assert(route("GET", "/v1/write-tier-grants", "server_id=srv1&team_id=910001", NULL, out,
                sizeof(out)) == 200);
   assert(stub_last_had_subject == 0);

   /* KEY BOUNDARIES. Each of these contains the literal text "team_id=" or "server_id="
    * and defines neither: a strstr-based parser would accept them, and team_id selects an
    * authorization scope. */
   const char *smuggled[] = {"server_id=srv1&my_team_id=910001", "xserver_id=srv1&team_id=1",
                             "server_id=srv1&team_id_extra=910001"};
   for (size_t i = 0; i < sizeof(smuggled) / sizeof(smuggled[0]); ++i)
   {
      reset();
      assert(route("GET", "/v1/write-tier-grants", smuggled[i], NULL, out, sizeof(out)) == 400);
      assert(stub_list_calls == 0);
   }

   /* Missing or unusable parameters, none of which reach the database. */
   const char *bad_qs[] = {NULL,
                           "",
                           "server_id=srv1",
                           "team_id=910001",
                           "server_id=srv1&team_id=0",
                           "server_id=srv1&team_id=-5",
                           "server_id=srv1&team_id=910001.9",
                           "server_id=srv1&team_id=910001abc",
                           "server_id=has space&team_id=1",
                           "server_id=srv1&team_id=1&subject=has space"};
   for (size_t i = 0; i < sizeof(bad_qs) / sizeof(bad_qs[0]); ++i)
   {
      reset();
      int st = route("GET", "/v1/write-tier-grants", bad_qs[i], NULL, out, sizeof(out));
      if (st != 400 || stub_list_calls != 0)
      {
         fprintf(stderr, "list accepted a bad query (status %d, calls %d): %s\n", st,
                 stub_list_calls, bad_qs[i] ? bad_qs[i] : "(null)");
         assert(0);
      }
   }
}

static void test_small_buffer(void)
{
   char tiny[24] = "";
   reset();
   stub_report.changed = 1;
   /* A response that will not fit must be an error, not a truncated JSON document that a
    * caller would parse as something else. */
   int st = route("POST", "/v1/write-tier-grants/set", NULL,
                  "{\"server_id\":\"s\",\"team_id\":1,\"subject\":\"owner\",\"tier\":\"data\","
                  "\"granted_by\":\"o\"}",
                  tiny, (int)sizeof(tiny));
   assert(st == 500);
   assert(strstr(tiny, "error"));
}

/* --- The subject arrives PERCENT-ESCAPED. kb_client_query_escape() escapes it before it goes
 * into the query string, because an oidc:<iss>:<sub> subject legitimately contains ':' and may
 * contain a literal '%3A' of its own. Reading it back raw made every prefixed subject fail the
 * canonical-identity check -- `oidc:test:alice` arrives as `oidc%3Atest%3Aalice`, which has no
 * ':' at all and is therefore judged a bare username, where '%' is forbidden. `show` and
 * `list --subject` 400'd for every federated identity while a bare username worked. Found by
 * scripts/run-grant-explore-live.sh probing set/show for the same subject. --- */
static void test_list_subject_is_percent_decoded(void)
{
   char out[8192] = "";

   /* The exact wire form the client produces for oidc:iss:alice. */
   reset();
   row(0, "oidc:iss:alice", KB_IDENTITY_TIER_DATA, "");
   assert(route("GET", "/v1/write-tier-grants",
                "server_id=srv1&team_id=910001&subject=oidc%3Aiss%3Aalice", NULL, out,
                sizeof(out)) == 200);
   /* Decoded before validation AND pushed down to the query, not filtered afterwards. */
   assert(!strcmp(stub_last_subject, "oidc:iss:alice"));

   /* cert: subjects too -- '=' is escaped by the client even though the grammar allows it raw. */
   reset();
   row(0, "cert:CN=aimee-ca:a1b", KB_IDENTITY_TIER_FULL, "");
   assert(route("GET", "/v1/write-tier-grants",
                "server_id=srv1&team_id=910001&subject=cert%3ACN%3Daimee-ca%3Aa1b", NULL, out,
                sizeof(out)) == 200);
   assert(!strcmp(stub_last_subject, "cert:CN=aimee-ca:a1b"));

   /* EXACTLY ONE level of decoding. A subject whose identity contains a literal '%3A' is
    * escaped to '%253A' and must come back as '%3A' -- decoding twice would turn it into ':'
    * and silently address a DIFFERENT principal. */
   reset();
   row(0, "oidc:a%3Ab:c%25d", KB_IDENTITY_TIER_DATA, "");
   assert(route("GET", "/v1/write-tier-grants",
                "server_id=srv1&team_id=910001&subject=oidc%3Aa%253Ab%3Ac%2525d", NULL, out,
                sizeof(out)) == 200);
   assert(!strcmp(stub_last_subject, "oidc:a%3Ab:c%25d"));

   /* A bare username still works: it needs no escaping and must not be altered. */
   reset();
   row(0, "alice", KB_IDENTITY_TIER_DATA, "");
   assert(route("GET", "/v1/write-tier-grants", "server_id=srv1&team_id=910001&subject=alice", NULL,
                out, sizeof(out)) == 200);
   assert(!strcmp(stub_last_subject, "alice"));
}

static void test_list_malformed_subject_is_refused_not_widened(void)
{
   char out[8192] = "";

   /* A malformed escape must be REFUSED. Demoting it to "no subject filter" would answer a
    * request about one principal by listing every grant on the server -- a wider disclosure
    * than was asked for, and a wrong answer. */
   const char *bad[] = {
       "server_id=srv1&team_id=910001&subject=oidc%3",  /* truncated escape */
       "server_id=srv1&team_id=910001&subject=oidc%ZZ", /* non-hex escape */
       "server_id=srv1&team_id=910001&subject=%",       /* a lone percent */
       "server_id=srv1&team_id=910001&subject=a%00b",   /* NUL would truncate */
       "server_id=srv1&team_id=910001&subject=a%0Ab",   /* a control byte */
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      reset();
      row(0, "oidc:iss:alice", KB_IDENTITY_TIER_DATA, "");
      int st = route("GET", "/v1/write-tier-grants", bad[i], NULL, out, sizeof(out));
      assert(st == 400);
      /* and it must not have reached the query at all */
      assert(stub_last_subject[0] == '\0');
   }
}

int main(void)
{
   test_not_our_routes();
   test_methods();
   test_set();
   test_set_rejects();
   test_actor_scope();
   test_db_failures();
   test_revoke();
   test_list();
   test_list_subject_is_percent_decoded();
   test_list_malformed_subject_is_refused_not_widened();
   test_small_buffer();
   printf("test_kb_http_grants: ok\n");
   return 0;
}
