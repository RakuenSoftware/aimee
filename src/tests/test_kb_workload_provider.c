#include "kb/kb_workload_helper_posix.h"
#include "kb/kb_workload_jwt.h"
#include "kb/kb_workload_proof.h"
#include "kb/kb_workload_wire.h"
#include "kb_workload_provider.h"

#include <assert.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct kb_workload_proof_key
{
   int marker;
};

static kb_workload_helper_result_t g_invoke_result = KB_WORKLOAD_HELPER_OK;
static kb_workload_result_t g_wire_status = KB_WORKLOAD_OK;
static kb_workload_result_t g_jwt_result = KB_WORKLOAD_OK;
static int g_bad_frame, g_bad_anchor, g_bad_proof;
static kb_workload_helper_result_t g_jwks_open_result = KB_WORKLOAD_HELPER_OK;
static int g_invoke_count, g_jwks_open_count, g_cancel_during_invoke;

static void put_u32(unsigned char out[4], uint32_t value)
{
   out[0] = (unsigned char)(value >> 24);
   out[1] = (unsigned char)(value >> 16);
   out[2] = (unsigned char)(value >> 8);
   out[3] = (unsigned char)value;
}

static void field(unsigned char *frame, size_t *offset, const void *data, size_t len)
{
   put_u32(frame + *offset, (uint32_t)len);
   *offset += 4;
   memcpy(frame + *offset, data, len);
   *offset += len;
}

kb_workload_helper_result_t mock_checked_root_file_open(const char *path, int require_exec,
                                                        int *fd_out)
{
   assert(path && fd_out);
   if (!require_exec && !strcmp(path, "/jwks"))
   {
      ++g_jwks_open_count;
      if (g_jwks_open_result != KB_WORKLOAD_HELPER_OK)
         return g_jwks_open_result;
   }
   static const unsigned char spki[] = {1, 2, 3};
   static const unsigned char jwks[] = {'{', '}'};
   const unsigned char *bytes = strstr(path, "proof") ? spki : jwks;
   size_t len = strstr(path, "proof") ? sizeof(spki) : sizeof(jwks);
   int pipefd[2];
   if (pipe(pipefd) != 0 || write(pipefd[1], bytes, len) != (ssize_t)len)
      return KB_WORKLOAD_HELPER_UNAVAILABLE;
   close(pipefd[1]);
   *fd_out = pipefd[0];
   return KB_WORKLOAD_HELPER_OK;
}

kb_workload_helper_result_t mock_helper_invoke(int helper_fd, const unsigned char *request,
                                               size_t request_len, unsigned char *response,
                                               size_t response_cap, size_t *response_len,
                                               int timeout_ms)
{
   (void)helper_fd;
   ++g_invoke_count;
   assert(request && request_len >= KB_WORKLOAD_WIRE_HEADER_LEN && timeout_ms == 1000);
   if (g_cancel_during_invoke)
      assert(pthread_cancel(pthread_self()) == 0);
   if (g_invoke_result != KB_WORKLOAD_HELPER_OK)
      return g_invoke_result;
   if (g_bad_frame)
   {
      memcpy(response, "bad", 3);
      *response_len = 3;
      return KB_WORKLOAD_HELPER_OK;
   }
   assert(response_cap >= 256);
   static const unsigned char magic[8] = {'A', 'I', 'M', 'E', 'E', 'W', 'I', '1'};
   memcpy(response, magic, 8);
   response[8] = request[8];
   response[9] = (unsigned char)g_wire_status;
   response[10] = response[11] = 0;
   size_t offset = KB_WORKLOAD_WIRE_HEADER_LEN;
   if (g_wire_status == KB_WORKLOAD_OK)
   {
      static const unsigned char token[] = "token";
      unsigned char proof_anchor[32], custody_anchor[32], proof[8];
      memset(proof_anchor, g_bad_anchor ? 0x22 : 0x11, sizeof(proof_anchor));
      memset(custody_anchor, 0x33, sizeof(custody_anchor));
      memset(proof, 0x44, sizeof(proof));
      field(response, &offset, token, sizeof(token) - 1);
      field(response, &offset, proof_anchor, sizeof(proof_anchor));
      field(response, &offset, custody_anchor, sizeof(custody_anchor));
      field(response, &offset, proof, sizeof(proof));
      if (request[8] == KB_WORKLOAD_OP_WRAP)
         field(response, &offset, "wrapped", 7);
      else if (request[8] == KB_WORKLOAD_OP_UNWRAP)
         field(response, &offset, "plain", 5);
   }
   put_u32(response + 12, (uint32_t)(offset - KB_WORKLOAD_WIRE_HEADER_LEN));
   *response_len = offset;
   return KB_WORKLOAD_HELPER_OK;
}

int mock_proof_key_load(const unsigned char *der, size_t len, kb_workload_proof_key_t **out)
{
   assert(der && len == 3);
   *out = calloc(1, sizeof(**out));
   return *out ? 0 : -1;
}

void mock_proof_key_close(kb_workload_proof_key_t *key)
{
   free(key);
}

int mock_proof_anchor(const kb_workload_proof_key_t *key, unsigned char out[32])
{
   assert(key);
   memset(out, 0x11, 32);
   return 0;
}

int mock_proof_verify(const kb_workload_proof_key_t *key, kb_workload_operation_t operation,
                      const unsigned char challenge[32], const unsigned char binding[32],
                      const unsigned char *token, size_t token_len,
                      const unsigned char proof_anchor[32], const unsigned char custody_anchor[32],
                      const void *request_data, size_t request_len, const void *response_data,
                      size_t response_len, const unsigned char *proof, size_t proof_len)
{
   assert(challenge && challenge[0] == 1 && binding && binding[0] == 2);
   (void)custody_anchor;
   if (operation == KB_WORKLOAD_OP_WRAP)
      assert(request_data && request_len == 6 && !memcmp(request_data, "secret", 6));
   else if (operation == KB_WORKLOAD_OP_UNWRAP)
      assert(request_data && request_len == 6 && !memcmp(request_data, "cipher", 6));
   else
      assert(!request_data && request_len == 0);
   (void)response_data;
   (void)response_len;
   assert(key && token && token_len == 5 && proof && proof_len == 8 && proof_anchor[0] == 0x11);
   return g_bad_proof ? -1 : 0;
}

kb_workload_result_t mock_jwt_validate(const void *token, size_t token_len, const void *jwks,
                                       size_t jwks_len, const char *issuer, const char *audience,
                                       uint64_t now, uint32_t max_age, kb_workload_identity_t *out)
{
   assert(token && token_len == 5 && jwks && jwks_len == 2 && issuer && audience && now &&
          max_age == 300);
   memset(out, 0, sizeof(*out));
   if (g_jwt_result != KB_WORKLOAD_OK)
      return g_jwt_result;
   strcpy(out->issuer, issuer);
   strcpy(out->subject, "spiffe://example/workload");
   out->issued_at = now - 1;
   out->expires_at = now + 60;
   memset(out->token_hash, 0x55, sizeof(out->token_hash));
   return KB_WORKLOAD_OK;
}

static kb_workload_provider_config_t config(void)
{
   return (kb_workload_provider_config_t){.kind = KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1,
                                          .helper_path = "/helper",
                                          .jwks_path = "/jwks",
                                          .proof_spki_path = "/proof",
                                          .expected_issuer = "https://issuer.example",
                                          .expected_audience = "aimee",
                                          .max_token_age_seconds = 300,
                                          .helper_timeout_ms = 1000};
}

static void reset(void)
{
   g_invoke_result = KB_WORKLOAD_HELPER_OK;
   g_wire_status = KB_WORKLOAD_OK;
   g_jwt_result = KB_WORKLOAD_OK;
   g_jwks_open_result = KB_WORKLOAD_HELPER_OK;
   g_bad_frame = g_bad_anchor = g_bad_proof = 0;
   g_cancel_during_invoke = 0;
}

typedef struct
{
   kb_workload_provider_t *provider;
   unsigned char challenge[32];
   unsigned char binding[32];
   kb_workload_identity_t identity;
} cancel_call_t;

static void *cancel_call(void *opaque)
{
   cancel_call_t *call = opaque;
   kb_workload_result_t result =
       kb_workload_attest(call->provider, call->challenge, call->binding, &call->identity);
   return (void *)(uintptr_t)(result + 1);
}

int main(void)
{
   kb_workload_provider_t *provider = (void *)1;
   kb_workload_provider_config_t cfg = config();
   cfg.kind = KB_WORKLOAD_PROVIDER_NONE;
   assert(kb_workload_provider_open(&cfg, &provider) == KB_WORKLOAD_DISABLED && !provider);
   cfg.kind = KB_WORKLOAD_PROVIDER_TPM2_V1;
   assert(kb_workload_provider_open(&cfg, &provider) == KB_WORKLOAD_DISABLED && !provider);
   cfg.kind = KB_WORKLOAD_PROVIDER_PKCS11_V1;
   assert(kb_workload_provider_open(&cfg, &provider) == KB_WORKLOAD_DISABLED && !provider);
   cfg = config();
   cfg.helper_timeout_ms = 5001;
   assert(kb_workload_provider_open(&cfg, &provider) == KB_WORKLOAD_INVALID && !provider);
   cfg = config();
   cfg.jwks_path = "/bad//jwks";
   assert(kb_workload_provider_open(&cfg, &provider) == KB_WORKLOAD_INVALID && !provider);

   cfg = config();
   assert(kb_workload_provider_open(&cfg, &provider) == KB_WORKLOAD_OK && provider);
   assert(g_jwks_open_count == 1); /* constructor probe */
   unsigned char challenge[32] = {1}, binding[32] = {2};
   kb_workload_identity_t identity;
   memset(&identity, 0xa5, sizeof(identity));
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_OK);
   assert(!strcmp(identity.subject, "spiffe://example/workload") &&
          identity.proof_anchor_id[0] == 0x11 && identity.custody_anchor_id[0] == 0x33);

   unsigned char output[KB_WORKLOAD_WIRE_CIPHER_MAX];
   size_t output_len = 99;
   assert(kb_workload_wrap(provider, challenge, binding, "secret", 6, &identity, output,
                           sizeof(output), &output_len) == KB_WORKLOAD_OK);
   assert(output_len == 7 && !memcmp(output, "wrapped", 7));
   assert(kb_workload_unwrap(provider, challenge, binding, "cipher", 6, &identity, output,
                             sizeof(output), &output_len) == KB_WORKLOAD_OK);
   assert(output_len == 5 && !memcmp(output, "plain", 5));
   assert(g_jwks_open_count == 4); /* one fresh checked read per operation */

   memcpy(output + 1, "secret", 6);
   output_len = 99;
   assert(kb_workload_wrap(provider, challenge, binding, output + 1, 6, &identity, output,
                           sizeof(output), &output_len) == KB_WORKLOAD_OK);
   assert(output_len == 7 && !memcmp(output, "wrapped", 7));

   int invokes_before_small_cap = g_invoke_count;
   memset(&identity, 0xa5, sizeof(identity));
   output[0] = 0xa5;
   output_len = 99;
   assert(kb_workload_wrap(provider, challenge, binding, "secret", 6, &identity, output,
                           KB_WORKLOAD_WIRE_CIPHER_MAX - 1, &output_len) == KB_WORKLOAD_INVALID);
   assert(g_invoke_count == invokes_before_small_cap && output_len == 0 && output[0] == 0 &&
          identity.issuer[0] == 0);

   unsigned char *large_output = malloc(KB_WORKLOAD_WIRE_FRAME_MAX + 1U);
   assert(large_output);
   output_len = 99;
   assert(kb_workload_wrap(provider, challenge, binding, "secret", 6, &identity, large_output,
                           KB_WORKLOAD_WIRE_FRAME_MAX + 1U, &output_len) == KB_WORKLOAD_OK);
   assert(output_len == 7 && !memcmp(large_output, "wrapped", 7));
   free(large_output);

   reset();
   g_wire_status = KB_WORKLOAD_DISABLED;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_DISABLED);
   reset();
   g_invoke_result = KB_WORKLOAD_HELPER_TIMEOUT;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_UNAVAILABLE);
   reset();
   g_invoke_result = KB_WORKLOAD_HELPER_INVALID;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_INTEGRITY);
   reset();
   g_bad_frame = 1;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_INTEGRITY);
   reset();
   g_bad_anchor = 1;
   memset(output, 0xa5, sizeof(output));
   output_len = 99;
   assert(kb_workload_wrap(provider, challenge, binding, "secret", 6, &identity, output,
                           sizeof(output), &output_len) == KB_WORKLOAD_INTEGRITY);
   assert(output_len == 0 && output[0] == 0 && identity.issuer[0] == 0);
   reset();
   g_bad_proof = 1;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_INTEGRITY);
   reset();
   g_jwt_result = KB_WORKLOAD_INVALID;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_INTEGRITY);
   reset();
   g_jwks_open_result = KB_WORKLOAD_HELPER_UNAVAILABLE;
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_UNAVAILABLE);
   reset();
   cancel_call_t cancel = {.provider = provider};
   memcpy(cancel.challenge, challenge, sizeof(challenge));
   memcpy(cancel.binding, binding, sizeof(binding));
   pthread_t cancel_thread;
   g_cancel_during_invoke = 1;
   assert(pthread_create(&cancel_thread, NULL, cancel_call, &cancel) == 0);
   void *cancel_result = NULL;
   assert(pthread_join(cancel_thread, &cancel_result) == 0);
   assert(cancel_result == PTHREAD_CANCELED);
   reset();
   assert(kb_workload_attest(provider, challenge, binding, &identity) == KB_WORKLOAD_OK);

   kb_workload_provider_close(provider);
   puts("kb_workload_provider: all tests passed");
   return 0;
}
