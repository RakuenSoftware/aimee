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
#include "db2/write_tier_grant.h"

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

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void reset(void)
{
   stub_rc = 0;
   stub_set_calls = stub_revoke_calls = stub_list_calls = 0;
   stub_last_include_revoked = -1;
   stub_last_had_subject = -1;
   stub_lookup_calls = 0;
   stub_lookup_rc = DB2_WRITE_TIER_GRANT_NONE;
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

   /* A NEGATIVE TENANCY CODE means the backend is not Postgres — a deployment fault, not
    * an authorization one. Collapsing the two would send an operator to debug credentials
    * when the answer is that RLS cannot be enforced at all on this backend. */
   reset();
   stub_rc = -42;
   assert(route("POST", "/v1/write-tier-grants/set", NULL, good, out, sizeof(out)) == 503);
   assert(strstr(out, "postgres backend"));

   reset();
   stub_rc = -42;
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
   stub_lookup_rc = -42;
   assert(route("POST", "/v1/write-tier-grants/revoke", NULL, good, out, sizeof(out)) == 503);
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

int main(void)
{
   test_not_our_routes();
   test_methods();
   test_set();
   test_set_rejects();
   test_db_failures();
   test_revoke();
   test_list();
   test_small_buffer();
   printf("test_kb_http_grants: ok\n");
   return 0;
}
