#include "server/server_mgmt_endpoint.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   char order[32];
   int token, staple, checkpoint, jti, writes, intent, apply, outcome;
} fixture_t;

static void mark(fixture_t *f, char c)
{
   size_t n = strlen(f->order);
   f->order[n] = c;
   f->order[n + 1] = 0;
}
static int token(void *v, const server_mgmt_endpoint_request_t *r, const char *digest,
                 server_mgmt_token_claims_t *c)
{
   fixture_t *f = v;
   mark(f, 'T');
   assert(strlen(digest) == 64 && r->peer);
   snprintf(c->capability, sizeof(c->capability), "%s", "remote_writes");
   snprintf(c->subject, sizeof(c->subject), "%s", "operator:7");
   snprintf(c->jti, sizeof(c->jti), "%s", "jti-7");
   return f->token;
}
static int staple(void *v, const server_mgmt_endpoint_request_t *r, uint64_t *generation,
                  char digest[65])
{
   fixture_t *f = v;
   mark(f, 'S');
   assert(r->staple_len == 2);
   *generation = 9;
   memset(digest, 'a', 64);
   digest[64] = 0;
   return f->staple;
}
static server_mgmt_checkpoint_result_t checkpoint(void *v, const server_mgmt_endpoint_request_t *r,
                                                  const server_mgmt_token_claims_t *c,
                                                  uint64_t generation, const char *digest)
{
   fixture_t *f = v;
   mark(f, 'C');
   assert(r && c && generation == 9 && strlen(digest) == 64);
   return (server_mgmt_checkpoint_result_t)f->checkpoint;
}
static server_mgmt_endpoint_jti_result_t jti(void *v, const server_mgmt_endpoint_request_t *r,
                                             const server_mgmt_token_claims_t *c)
{
   fixture_t *f = v;
   mark(f, 'J');
   assert(r && !strcmp(c->jti, "jti-7"));
   return (server_mgmt_endpoint_jti_result_t)f->jti;
}
static int writes(void *v)
{
   fixture_t *f = v;
   mark(f, 'W');
   return f->writes;
}
static int audit(void *v, const server_mgmt_token_claims_t *c, const server_mgmt_action_t *a,
                 int outcome, int status)
{
   fixture_t *f = v;
   mark(f, outcome ? 'O' : 'I');
   assert(!strcmp(c->subject, "operator:7") && !strcmp(a->agent, "alpha"));
   return outcome ? f->outcome : f->intent;
}
static int apply(void *v, const server_mgmt_action_t *a)
{
   fixture_t *f = v;
   mark(f, 'A');
   assert(!strcmp(a->action, "agent.enable"));
   return f->apply;
}

static void run(fixture_t *f, int status, const char *result, const char *effect, const char *order)
{
   server_tls_peer_cert_t peer = {.management_profile = 1};
   snprintf(peer.cn, sizeof(peer.cn), "%s", "p5-kb-management");
   server_mgmt_endpoint_request_t rq = {
       .body = "{\"agent\":\"alpha\",\"action\":\"agent.enable\"}",
       .body_len = 42,
       .jwt = "x",
       .jwt_len = 1,
       .staple = "{}",
       .staple_len = 2,
       .expected_issuer = "https://kb",
       .server_id = "server-a",
       .peer = &peer,
       .local_fingerprint = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       .now = 50,
   };
   rq.body_len = strlen(rq.body);
   server_mgmt_endpoint_deps_t d = {token, staple, checkpoint, jti, writes, audit, apply, f};
   server_mgmt_endpoint_result_t got;
   assert(server_mgmt_endpoint_dispatch(&rq, &d, &got) == status);
   assert(got.status == status && !strcmp(got.result, result) && !strcmp(got.effect, effect));
   assert(!strcmp(f->order, order));
   char wire[80];
   assert(server_mgmt_endpoint_render(&got, wire, sizeof(wire)) > 0);
}

int main(void)
{
   server_mgmt_action_t a;
   const char *valid = "{\"agent\":\"a-1\",\"action\":\"agent.disable\"}";
   assert(server_mgmt_action_parse(valid, strlen(valid), &a) == 0);
   /* Both spellings of the roster action are accepted: the ops were renamed to
    * `model.*`, but this is an INGRESS contract and an external control plane
    * still emitting `agent.*` is not redeployed in lockstep. Each is echoed
    * verbatim into the canonical form, so neither is rewritten into the other. */
   const char *renamed = "{\"agent\":\"a-1\",\"action\":\"model.disable\"}";
   assert(server_mgmt_action_parse(renamed, strlen(renamed), &a) == 0);
   assert(!strcmp(a.canonical, "{\"action\":\"model.disable\",\"agent\":\"a-1\"}"));
   const char *renamed_on = "{\"agent\":\"a-1\",\"action\":\"model.enable\"}";
   assert(server_mgmt_action_parse(renamed_on, strlen(renamed_on), &a) == 0);
   assert(server_mgmt_action_parse(valid, strlen(valid), &a) == 0);
   const char *nul_action = "{\"action\":\"agent.enable\\u0000junk\",\"agent\":\"alpha\"}";
   const char *nul_agent = "{\"action\":\"agent.enable\",\"agent\":\"alpha\\u0000other\"}";
   assert(server_mgmt_action_parse(nul_action, strlen(nul_action), &a) != 0);
   assert(server_mgmt_action_parse(nul_agent, strlen(nul_agent), &a) != 0);
   assert(!strcmp(a.canonical, "{\"action\":\"agent.disable\",\"agent\":\"a-1\"}"));
   assert(strlen(a.digest) == 64);
   const char *bad[] = {
       "{}", "{\"action\":\"agent.enable\"}", "{\"action\":\"agent.enable\",\"agent\":\"a/b\"}",
       "{\"action\":\"agent.enable\",\"agent\":\"a\",\"x\":\"y\"}",
       "{\"action\":\"agent.enable\",\"action\":\"agent.enable\",\"agent\":\"a\"}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
      assert(server_mgmt_action_parse(bad[i], strlen(bad[i]), &a) != 0);

   fixture_t f = {.writes = 2};
   run(&f, 200, "succeeded", "applied", "TSCJWIAO");
   f = (fixture_t){.writes = 2, .jti = SERVER_MGMT_JTI_REPLAY};
   run(&f, 403, "denied", "none", "TSCJ");
   f = (fixture_t){.writes = 2, .checkpoint = SERVER_MGMT_CHECKPOINT_UNAVAILABLE};
   run(&f, 500, "failed", "none", "TSC");
   f = (fixture_t){.writes = 1};
   run(&f, 403, "denied", "none", "TSCJW");
   f = (fixture_t){.writes = 2, .outcome = -1};
   run(&f, 502, "indeterminate", "unknown", "TSCJWIAO");
   f = (fixture_t){.writes = 2, .apply = 1};
   run(&f, 500, "failed", "none", "TSCJWIAO");
   f = (fixture_t){.writes = 2, .apply = 2};
   run(&f, 502, "indeterminate", "unknown", "TSCJWIAO");
   puts("server management endpoint tests passed");
   return 0;
}
