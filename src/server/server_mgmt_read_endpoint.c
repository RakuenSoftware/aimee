#include "server_mgmt_read_endpoint.h"

#include <openssl/crypto.h>
#include <string.h>

static int lower_hex_64(const char *s)
{
   if (!s || strlen(s) != 64)
      return 0;
   for (size_t i = 0; i < 64; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static server_mgmt_read_result_t closed_result(server_mgmt_read_result_t result)
{
   return result >= SERVER_MGMT_READ_OK && result <= SERVER_MGMT_READ_UNAVAILABLE
              ? result
              : SERVER_MGMT_READ_UNAVAILABLE;
}

static server_mgmt_read_result_t fail(char *out, size_t cap, size_t *out_len,
                                      server_mgmt_read_result_t result)
{
   if (out && cap)
      out[0] = 0;
   if (out_len)
      *out_len = 0;
   return result;
}

server_mgmt_read_result_t server_mgmt_read_dispatch(const server_mgmt_read_request_t *rq,
                                                    const server_mgmt_read_deps_t *deps, char *out,
                                                    size_t cap, size_t *out_len)
{
   if (!rq || !deps || !out || !cap || !out_len || !deps->verify_and_consume_status ||
       !deps->verify_token || !deps->consume_jti || !deps->load_agents ||
       !deps->verify_checkpoint || !rq->jwt || !rq->jwt_len || !rq->staple || !rq->staple_len ||
       !rq->expected_issuer || !rq->server_id || !rq->peer || !rq->peer->management_profile ||
       strcmp(rq->peer->cn, "p5-kb-management") || !rq->local_issuer || !rq->local_serial ||
       !rq->local_fingerprint || !rq->publication_generation || rq->now < 0)
      return fail(out, cap, out_len, SERVER_MGMT_READ_INTEGRITY);

   server_mgmt_read_status_proof_t proof;
   memset(&proof, 0, sizeof(proof));
   server_mgmt_read_result_t rc = deps->verify_and_consume_status(deps->ctx, rq, &proof);
   if (rc != SERVER_MGMT_READ_OK)
      return fail(out, cap, out_len, closed_result(rc));
   if (!proof.revocation_generation || !lower_hex_64(proof.staple_sha256))
      return fail(out, cap, out_len, SERVER_MGMT_READ_INTEGRITY);

   server_mgmt_token_claims_t claims;
   memset(&claims, 0, sizeof(claims));
   rc = deps->verify_token(deps->ctx, rq, &claims);
   if (rc != SERVER_MGMT_READ_OK)
      return fail(out, cap, out_len, closed_result(rc));
   if (strcmp(claims.issuer, rq->expected_issuer) || strcmp(claims.capability, "remote_reads") ||
       strcmp(claims.audience, rq->server_id) || strcmp(claims.peer_issuer, rq->peer->issuer) ||
       strcmp(claims.peer_serial, rq->peer->serial_norm) ||
       strcmp(claims.peer_fingerprint, rq->peer->fingerprint) || claims.team_id <= 0)
      return fail(out, cap, out_len, SERVER_MGMT_READ_INTEGRITY);

   server_mgmt_read_digest_input_t digest_input = {
       rq->server_id,
       claims.team_id,
       proof.nonce,
       rq->peer->issuer,
       rq->peer->serial_norm,
       rq->local_issuer,
       rq->local_serial,
       proof.revocation_generation,
       rq->publication_generation,
   };
   char expected_digest[65];
   if (server_mgmt_read_digest(&digest_input, expected_digest) != 0 ||
       strlen(claims.request_sha256) != 64 ||
       CRYPTO_memcmp(expected_digest, claims.request_sha256, 64) != 0)
      return fail(out, cap, out_len, SERVER_MGMT_READ_INTEGRITY);

   server_mgmt_endpoint_request_t jti_rq = {
       .jwt = rq->jwt,
       .jwt_len = rq->jwt_len,
       .staple = rq->staple,
       .staple_len = rq->staple_len,
       .expected_issuer = rq->expected_issuer,
       .server_id = rq->server_id,
       .peer = rq->peer,
       .local_fingerprint = rq->local_fingerprint,
       .now = rq->now,
   };
   server_mgmt_endpoint_jti_result_t jti = deps->consume_jti(deps->ctx, &jti_rq, &claims);
   if (jti == SERVER_MGMT_JTI_REPLAY)
      return fail(out, cap, out_len, SERVER_MGMT_READ_CONFLICT);
   if (jti != SERVER_MGMT_JTI_OK)
      return fail(out, cap, out_len, SERVER_MGMT_READ_UNAVAILABLE);

   server_mgmt_read_agent_t agents[SERVER_MGMT_READ_AGENT_MAX];
   size_t count = 0;
   memset(agents, 0, sizeof(agents));
   if (deps->load_agents(deps->ctx, agents, SERVER_MGMT_READ_AGENT_MAX, &count) != 0 ||
       count > SERVER_MGMT_READ_AGENT_MAX)
      return fail(out, cap, out_len, SERVER_MGMT_READ_UNAVAILABLE);
   int n = server_mgmt_read_project(rq->server_id, claims.team_id, agents, count, out, cap);
   if (n < 0)
      return fail(out, cap, out_len, SERVER_MGMT_READ_UNAVAILABLE);

   rc = deps->verify_checkpoint(deps->ctx, rq, &claims, &proof);
   if (rc != SERVER_MGMT_READ_OK)
      return fail(out, cap, out_len, closed_result(rc));
   *out_len = (size_t)n;
   return SERVER_MGMT_READ_OK;
}
