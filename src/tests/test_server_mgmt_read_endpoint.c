#include "server/server_mgmt_read_endpoint.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int step;
   int status_calls, token_calls, jti_calls, load_calls, checkpoint_calls;
   server_mgmt_read_result_t status_result, token_result, checkpoint_result;
   server_mgmt_endpoint_jti_result_t jti_result;
   int load_fail;
   char digest[65];
} fixture_t;

static server_mgmt_read_result_t status_cb(void *opaque, const server_mgmt_read_request_t *rq,
                                           server_mgmt_read_status_proof_t *out)
{
   fixture_t *f = opaque;
   assert(f->step++ == 0);
   f->status_calls++;
   if (f->status_result != SERVER_MGMT_READ_OK)
      return f->status_result;
   for (size_t i = 0; i < sizeof(out->nonce); ++i)
      out->nonce[i] = (unsigned char)i;
   out->revocation_generation = 7;
   memset(out->staple_sha256, 'a', 64);
   out->staple_sha256[64] = 0;
   return SERVER_MGMT_READ_OK;
}

static server_mgmt_read_result_t token_cb(void *opaque, const server_mgmt_read_request_t *rq,
                                          server_mgmt_token_claims_t *out)
{
   fixture_t *f = opaque;
   assert(f->step++ == 1);
   f->token_calls++;
   if (f->token_result != SERVER_MGMT_READ_OK)
      return f->token_result;
   out->team_id = 42;
   snprintf(out->issuer, sizeof(out->issuer), "%s", rq->expected_issuer);
   snprintf(out->audience, sizeof(out->audience), "%s", rq->server_id);
   snprintf(out->capability, sizeof(out->capability), "%s", "remote_reads");
   snprintf(out->request_sha256, sizeof(out->request_sha256), "%s", f->digest);
   snprintf(out->peer_issuer, sizeof(out->peer_issuer), "%s", rq->peer->issuer);
   snprintf(out->peer_serial, sizeof(out->peer_serial), "%s", rq->peer->serial_norm);
   snprintf(out->peer_fingerprint, sizeof(out->peer_fingerprint), "%s", rq->peer->fingerprint);
   return SERVER_MGMT_READ_OK;
}

static server_mgmt_endpoint_jti_result_t jti_cb(void *opaque,
                                                const server_mgmt_endpoint_request_t *rq,
                                                const server_mgmt_token_claims_t *claims)
{
   fixture_t *f = opaque;
   assert(f->step++ == 2 && claims->team_id == 42 && !strcmp(rq->server_id, "server-a"));
   f->jti_calls++;
   return f->jti_result;
}

static int load_cb(void *opaque, server_mgmt_read_agent_t *out, size_t cap, size_t *count)
{
   fixture_t *f = opaque;
   assert(f->step++ == 3 && cap == SERVER_MGMT_READ_AGENT_MAX);
   f->load_calls++;
   if (f->load_fail)
      return -1;
   snprintf(out[0].name, sizeof(out[0].name), "%s", "agent-a");
   snprintf(out[0].provider, sizeof(out[0].provider), "%s", "openai");
   snprintf(out[0].model, sizeof(out[0].model), "%s", "gpt-5.2");
   out[0].enabled = 1;
   out[0].delegate_available = 1;
   out[0].max_parallel = 2;
   *count = 1;
   return 0;
}

static server_mgmt_read_result_t checkpoint_cb(void *opaque, const server_mgmt_read_request_t *rq,
                                               const server_mgmt_token_claims_t *claims,
                                               const server_mgmt_read_status_proof_t *proof)
{
   fixture_t *f = opaque;
   assert(f->step++ == 4 && claims->team_id == 42 && proof->revocation_generation == 7 &&
          !strcmp(rq->local_serial, "10be"));
   f->checkpoint_calls++;
   return f->checkpoint_result;
}

static fixture_t fixture(void)
{
   fixture_t f;
   memset(&f, 0, sizeof(f));
   f.jti_result = SERVER_MGMT_JTI_OK;
   unsigned char nonce[32];
   for (size_t i = 0; i < sizeof(nonce); ++i)
      nonce[i] = (unsigned char)i;
   server_mgmt_read_digest_input_t in = {
       "server-a", 42, nonce, "/CN=kb-ca", "01af", "/CN=server-ca", "10be", 7, 9};
   assert(server_mgmt_read_digest(&in, f.digest) == 0);
   return f;
}

static server_mgmt_read_result_t run(fixture_t *f, char *out, size_t *out_len)
{
   static const server_tls_peer_cert_t peer = {
       .cn = "p5-kb-management",
       .issuer = "/CN=kb-ca",
       .serial_norm = "01af",
       .fingerprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
       .management_profile = 1,
   };
   server_mgmt_read_request_t rq = {
       "token",
       5,
       "staple",
       6,
       "https://kb.test",
       "server-a",
       &peer,
       "/CN=server-ca",
       "10be",
       "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
       9,
       100,
   };
   server_mgmt_read_deps_t deps = {status_cb, token_cb, jti_cb, load_cb, checkpoint_cb, f};
   return server_mgmt_read_dispatch(&rq, &deps, out, 4096, out_len);
}

int main(void)
{
   char out[4096];
   size_t out_len = 0;
   fixture_t f = fixture();
   assert(run(&f, out, &out_len) == SERVER_MGMT_READ_OK);
   assert(out_len == strlen(out) && strstr(out, "\"name\":\"agent-a\"") && f.step == 5);

   f = fixture();
   f.status_result = SERVER_MGMT_READ_CONFLICT;
   assert(run(&f, out, &out_len) == SERVER_MGMT_READ_CONFLICT);
   assert(!out_len && !out[0] && f.status_calls == 1 && !f.token_calls);

   f = fixture();
   f.digest[0] = f.digest[0] == 'a' ? 'b' : 'a';
   assert(run(&f, out, &out_len) == SERVER_MGMT_READ_INTEGRITY);
   assert(!f.jti_calls && !f.load_calls && !out_len);

   f = fixture();
   f.jti_result = SERVER_MGMT_JTI_REPLAY;
   assert(run(&f, out, &out_len) == SERVER_MGMT_READ_CONFLICT);
   assert(f.jti_calls == 1 && !f.load_calls && !out_len);

   f = fixture();
   f.load_fail = 1;
   assert(run(&f, out, &out_len) == SERVER_MGMT_READ_UNAVAILABLE);
   assert(f.load_calls == 1 && !f.checkpoint_calls && !out_len);

   f = fixture();
   f.checkpoint_result = SERVER_MGMT_READ_INTEGRITY;
   assert(run(&f, out, &out_len) == SERVER_MGMT_READ_INTEGRITY);
   assert(f.load_calls == 1 && f.checkpoint_calls == 1 && !out_len && !out[0]);
   puts("server management read endpoint tests passed");
   return 0;
}
