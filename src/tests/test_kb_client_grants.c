/* test_kb_client_grants.c — the server's client for kb's grant routes.
 *
 * The transport is stubbed, so what this pins is how the client INTERPRETS kb. That
 * interpretation is where an operator gets misled: every distinct outcome here leads to a
 * different next action, and collapsing any two of them sends somebody to fix the wrong
 * thing.
 *
 * Pinned in particular:
 *   - an UNREACHABLE kb is never reported as DENIED. That would be a lie about authority,
 *     and would have an operator editing grants that were never consulted.
 *   - a 503 (this backend cannot do grants at all) is not a 403 (you may not).
 *   - a 200 whose body lacks `changed` is a failure, not a silent "unchanged" — that would
 *     print "no change" for a mutation that happened.
 *   - a partial listing is REPORTED as partial, whether kb truncated it or the caller's
 *     buffer did. Presenting it as complete misrepresents who can write.
 *   - the subject is URL-escaped, because the oidc:<iss>:<sub> form legitimately contains
 *     percent-encoded characters that would otherwise corrupt the query.
 */
#include "kb_client_grants.h"

#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── The transport, stubbed ────────────────────────────────────────────────── */

static int stub_status;       /* what kb "answered" */
static const char *stub_body; /* its body, or NULL for none */
static char stub_last_path[2048];
static char stub_last_body[2048];
static int stub_calls;

static char *dup_or_null(const char *s)
{
   if (!s)
      return NULL;
   size_t n = strlen(s) + 1;
   char *p = malloc(n);
   if (p)
      memcpy(p, s, n);
   return p;
}

char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out)
{
   stub_calls++;
   (void)timeout_ms;
   snprintf(stub_last_path, sizeof(stub_last_path), "%s", path ? path : "");
   char *rendered = body ? cJSON_PrintUnformatted(body) : NULL;
   snprintf(stub_last_body, sizeof(stub_last_body), "%s", rendered ? rendered : "");
   free(rendered);
   if (status_out)
      *status_out = stub_status;
   return dup_or_null(stub_body);
}

char *kb_client_v1_get_json(const char *path, int timeout_ms, int *status_out)
{
   stub_calls++;
   (void)timeout_ms;
   snprintf(stub_last_path, sizeof(stub_last_path), "%s", path ? path : "");
   if (status_out)
      *status_out = stub_status;
   return dup_or_null(stub_body);
}

/* A deliberately simple escaper: it must at least encode the characters that would break a
 * query string, so a test asserting "the subject was escaped" is meaningful. */
char *kb_client_query_escape(const char *s)
{
   if (!s)
      return NULL;
   size_t n = strlen(s);
   char *out = malloc(n * 3 + 1);
   if (!out)
      return NULL;
   size_t j = 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.';
      if (safe)
         out[j++] = (char)c;
      else
      {
         out[j++] = '%';
         out[j++] = "0123456789ABCDEF"[(c >> 4) & 0xF];
         out[j++] = "0123456789ABCDEF"[c & 0xF];
      }
   }
   out[j] = '\0';
   return out;
}

static void reset(int status, const char *body)
{
   stub_status = status;
   stub_body = body;
   stub_calls = 0;
   stub_last_path[0] = stub_last_body[0] = '\0';
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_set_success(void)
{
   kb_client_grant_change_t ch;

   /* Created: changed, and had_previous is 0 because the body carried no previous_tier. */
   reset(200, "{\"changed\":true,\"was_revoked\":false,\"is_member\":true}");
   assert(kb_client_grant_set("srv1", 910001, "oidc:iss:alice", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_OK);
   assert(ch.changed == 1 && ch.was_revoked == 0 && ch.is_member == 1);
   assert(ch.had_previous == 0 && ch.previous_tier[0] == '\0');
   /* The request went to the right route with the right fields. */
   assert(!strcmp(stub_last_path, "/v1/write-tier-grants/set"));
   assert(strstr(stub_last_body, "\"server_id\":\"srv1\""));
   assert(strstr(stub_last_body, "\"team_id\":910001"));
   assert(strstr(stub_last_body, "\"subject\":\"oidc:iss:alice\""));
   assert(strstr(stub_last_body, "\"tier\":\"data\""));
   assert(strstr(stub_last_body, "\"granted_by\":\"owner\""));

   /* Re-tiered: previous_tier present, so had_previous is set. */
   reset(200, "{\"changed\":true,\"was_revoked\":false,\"previous_tier\":\"data\","
              "\"is_member\":true}");
   assert(kb_client_grant_set("srv1", 910001, "owner", "full", "owner", &ch) == KB_CLIENT_GRANT_OK);
   assert(ch.had_previous == 1 && !strcmp(ch.previous_tier, "data"));

   /* Un-revoked. */
   reset(200, "{\"changed\":true,\"was_revoked\":true,\"is_member\":false}");
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) == KB_CLIENT_GRANT_OK);
   assert(ch.was_revoked == 1 && ch.is_member == 0);

   /* An unchanged no-op is still a success. */
   reset(200, "{\"changed\":false,\"was_revoked\":false,\"previous_tier\":\"data\","
              "\"is_member\":true}");
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) == KB_CLIENT_GRANT_OK);
   assert(ch.changed == 0);
}

static void test_status_mapping(void)
{
   kb_client_grant_change_t ch;

   /* Each status is its own outcome. A 503 says grants cannot be administered on this
    * backend at all; a 403 says this caller may not. Sending an operator to check
    * credentials for a 503 wastes the one thing they have least of during an incident. */
   struct
   {
      int status;
      kb_client_grant_result_t want;
   } cases[] = {
       {400, KB_CLIENT_GRANT_INVALID},     {403, KB_CLIENT_GRANT_DENIED},
       {405, KB_CLIENT_GRANT_INVALID},     {503, KB_CLIENT_GRANT_BACKEND},
       {500, KB_CLIENT_GRANT_UNAVAILABLE}, {502, KB_CLIENT_GRANT_UNAVAILABLE},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      reset(cases[i].status, "{\"error\":\"nope\"}");
      kb_client_grant_result_t got =
          kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch);
      if (got != cases[i].want)
      {
         fprintf(stderr, "status %d mapped to %d, expected %d\n", cases[i].status, (int)got,
                 (int)cases[i].want);
         assert(0);
      }
      /* Nothing is reported about a grant that was not changed. */
      assert(ch.changed == 0 && ch.had_previous == 0);
   }

   /* AN UNREACHABLE kb: no status, no body. Must be UNAVAILABLE, never DENIED — the
    * request was never evaluated, so saying "denied" asserts an authority decision that
    * nothing made. */
   reset(0, NULL);
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_UNAVAILABLE);
   reset(0, NULL);
   int found = 1;
   assert(kb_client_grant_revoke("srv1", 910001, "owner", &found) == KB_CLIENT_GRANT_UNAVAILABLE);
   assert(found == 0); /* and it must not claim a grant existed */

   /* A 200 with NO BODY is unusable rather than a success, on ALL THREE calls. kb always
    * sends a body on success, so an empty one means the answer is unknown — and "unknown"
    * must not render as "unchanged", "no grant existed", or "nobody can write". This was a
    * real bug: the first version returned OK here because the status alone decided. */
   reset(200, NULL);
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_UNAVAILABLE);
   reset(200, NULL);
   int f2 = 1;
   assert(kb_client_grant_revoke("srv1", 910001, "owner", &f2) == KB_CLIENT_GRANT_UNAVAILABLE);
   {
      kb_client_grant_row_t r[2];
      size_t c = 99;
      int t = 0;
      reset(200, NULL);
      assert(kb_client_grant_list("srv1", 910001, NULL, 0, r, 2, &c, &t) ==
             KB_CLIENT_GRANT_UNAVAILABLE);
      assert(c == 0); /* and it must not look like an empty grant list */
   }
   /* A FAILURE status with no body still maps by status: the status is the information. */
   reset(403, NULL);
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_DENIED);
   reset(503, NULL);
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_BACKEND);

   /* A 200 whose body omits `changed` must NOT read as "unchanged": that would report no
    * change for a mutation that happened. */
   reset(200, "{\"is_member\":true}");
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_UNAVAILABLE);
   reset(200, "{\"changed\":\"yes\"}"); /* wrong type */
   assert(kb_client_grant_set("srv1", 910001, "owner", "data", "owner", &ch) ==
          KB_CLIENT_GRANT_UNAVAILABLE);
}

static void test_argument_validation(void)
{
   kb_client_grant_change_t ch;
   /* Refused locally, without a round trip — a malformed call is a client bug and must not
    * be sent for kb to reject. */
   reset(200, "{\"changed\":true}");
   assert(kb_client_grant_set(NULL, 1, "owner", "data", "o", &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_set("", 1, "owner", "data", "o", &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_set("s", 0, "owner", "data", "o", &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_set("s", -1, "owner", "data", "o", &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_set("s", 1, NULL, "data", "o", &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_set("s", 1, "owner", NULL, "o", &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_set("s", 1, "owner", "data", NULL, &ch) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_revoke(NULL, 1, "owner", NULL) == KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_revoke("s", 0, "owner", NULL) == KB_CLIENT_GRANT_INVALID);
   assert(stub_calls == 0);
}

static void test_revoke(void)
{
   int found = 0;
   reset(200, "{\"found\":true}");
   assert(kb_client_grant_revoke("srv1", 910001, "owner", &found) == KB_CLIENT_GRANT_OK);
   assert(found == 1);
   assert(!strcmp(stub_last_path, "/v1/write-tier-grants/revoke"));

   reset(200, "{\"found\":false}");
   assert(kb_client_grant_revoke("srv1", 910001, "owner", &found) == KB_CLIENT_GRANT_OK);
   assert(found == 0);

   /* A body without `found` defaults to 0: claiming a grant existed when kb did not say so
    * would tell an operator they closed access they never held. */
   reset(200, "{}");
   found = 1;
   assert(kb_client_grant_revoke("srv1", 910001, "owner", &found) == KB_CLIENT_GRANT_OK);
   assert(found == 0);
}

static void test_list(void)
{
   kb_client_grant_row_t rows[3];
   size_t count = 0;
   int truncated = 0;

   reset(200, "{\"grants\":["
              "{\"subject\":\"a\",\"tier\":\"data\",\"granted_by\":\"owner\","
              "\"created_at\":\"c1\",\"updated_at\":\"u1\"},"
              "{\"subject\":\"b\",\"tier\":\"full\",\"granted_by\":\"owner\","
              "\"created_at\":\"c2\",\"updated_at\":\"u2\",\"revoked_at\":\"r2\"}"
              "],\"truncated\":false}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_OK);
   assert(count == 2 && truncated == 0);
   assert(!strcmp(rows[0].subject, "a") && !strcmp(rows[0].tier, "data"));
   /* A live grant's revoked_at is empty, which is how live and revoked are told apart
    * without a separate flag. */
   assert(rows[0].revoked_at[0] == '\0');
   assert(!strcmp(rows[1].revoked_at, "r2"));
   /* include_revoked absent from the query when not asked for. */
   assert(!strstr(stub_last_path, "include_revoked"));
   assert(strstr(stub_last_path, "server_id=srv1") && strstr(stub_last_path, "team_id=910001"));

   /* include_revoked widens, and says so on the wire. */
   reset(200, "{\"grants\":[],\"truncated\":false}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 1, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_OK);
   assert(strstr(stub_last_path, "include_revoked=1"));
   assert(count == 0);

   /* THE SUBJECT IS ESCAPED. An oidc subject contains ':' and '%', which would otherwise
    * corrupt the query — and a corrupted filter silently lists the wrong rows. */
   reset(200, "{\"grants\":[],\"truncated\":false}");
   assert(kb_client_grant_list("srv1", 910001, "oidc:https%3A//i:alice", 0, rows, 3, &count,
                               &truncated) == KB_CLIENT_GRANT_OK);
   assert(strstr(stub_last_path, "subject="));
   assert(!strstr(stub_last_path, "subject=oidc:https")); /* raw colon must not appear */
   assert(strstr(stub_last_path, "%3A"));

   /* TRUNCATION IS REPORTED, from either side. kb saying so: */
   reset(200, "{\"grants\":[{\"subject\":\"a\",\"tier\":\"data\"}],\"truncated\":true}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_OK);
   assert(truncated == 1);
   /* ...and the caller's own buffer being too small, which kb cannot know about. Presenting
    * this as the complete set would misrepresent who can write to the server. */
   reset(200, "{\"grants\":[{\"subject\":\"a\",\"tier\":\"data\"},"
              "{\"subject\":\"b\",\"tier\":\"data\"},{\"subject\":\"c\",\"tier\":\"data\"}],"
              "\"truncated\":false}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 2, &count, &truncated) ==
          KB_CLIENT_GRANT_OK);
   assert(count == 2 && truncated == 1);

   /* A row missing its subject or tier is not a grant. The whole listing is refused rather
    * than silently under-reporting, because a short grant list reads as "fewer people can
    * write than actually can". */
   reset(200, "{\"grants\":[{\"tier\":\"data\"}],\"truncated\":false}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_UNAVAILABLE);
   reset(200, "{\"grants\":[{\"subject\":\"a\"}],\"truncated\":false}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_UNAVAILABLE);
   /* A body with no grants array at all is likewise unusable, not an empty answer. */
   reset(200, "{\"ok\":true}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_UNAVAILABLE);

   /* Status mapping applies to reads too. */
   reset(403, "{\"error\":\"nope\"}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_DENIED);
   reset(503, "{\"error\":\"backend\"}");
   assert(kb_client_grant_list("srv1", 910001, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_BACKEND);

   /* Bad arguments, no round trip. */
   reset(200, "{\"grants\":[]}");
   assert(kb_client_grant_list(NULL, 1, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_list("s", 0, NULL, 0, rows, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_list("s", 1, NULL, 0, NULL, 3, &count, &truncated) ==
          KB_CLIENT_GRANT_INVALID);
   assert(kb_client_grant_list("s", 1, NULL, 0, rows, 0, &count, &truncated) ==
          KB_CLIENT_GRANT_INVALID);
   assert(stub_calls == 0);
}

int main(void)
{
   test_set_success();
   test_status_mapping();
   test_argument_validation();
   test_revoke();
   test_list();
   printf("test_kb_client_grants: ok\n");
   return 0;
}
