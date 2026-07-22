#include "kb_workload_provider.h"

#include "kb_workload_helper_posix.h"
#include "kb_workload_jwt.h"
#include "kb_workload_proof.h"
#include "kb_workload_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKLOAD_CONFIG_TEXT_MAX 600U
#define WORKLOAD_JWKS_MAX        65536U

struct kb_workload_provider
{
   kb_workload_provider_kind_t kind;
   int helper_fd;
   char helper_path[PATH_MAX];
   char jwks_path[PATH_MAX];
   char proof_spki_path[PATH_MAX];
   char expected_issuer[WORKLOAD_CONFIG_TEXT_MAX + 1];
   char expected_audience[WORKLOAD_CONFIG_TEXT_MAX + 1];
   uint32_t max_token_age_seconds;
   uint32_t helper_timeout_ms;
   kb_workload_proof_key_t *proof_key;
   unsigned char proof_anchor_id[KB_WORKLOAD_ANCHOR_LEN];
   pthread_mutex_t mutex;
   int mutex_ready;
};

kb_workload_provider_kind_t kb_workload_provider_kind(const kb_workload_provider_t *provider)
{
   return provider ? provider->kind : KB_WORKLOAD_PROVIDER_NONE;
}

static int printable(const char *text, size_t max)
{
   if (!text)
      return 0;
   size_t n = strnlen(text, max + 1);
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)text[i] < 0x21 || (unsigned char)text[i] > 0x7e)
         return 0;
   return 1;
}

static int absolute_path(const char *path)
{
   if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) == PATH_MAX)
      return 0;
   const char *component = path + 1;
   for (;;)
   {
      const char *slash = strchr(component, '/');
      size_t length = slash ? (size_t)(slash - component) : strlen(component);
      if (!length || length > NAME_MAX || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.'))
         return 0;
      if (!slash)
         return 1;
      component = slash + 1;
   }
}

static kb_workload_result_t helper_result(kb_workload_helper_result_t result)
{
   switch (result)
   {
   case KB_WORKLOAD_HELPER_OK:
      return KB_WORKLOAD_OK;
   case KB_WORKLOAD_HELPER_INVALID:
      return KB_WORKLOAD_INVALID;
   case KB_WORKLOAD_HELPER_DISABLED:
      return KB_WORKLOAD_DISABLED;
   case KB_WORKLOAD_HELPER_INTEGRITY:
      return KB_WORKLOAD_INTEGRITY;
   default:
      return KB_WORKLOAD_UNAVAILABLE;
   }
}

static kb_workload_result_t checked_file_read(const char *path, unsigned char *out, size_t cap,
                                              size_t *out_len)
{
   *out_len = 0;
   int fd = -1;
   kb_workload_result_t result = helper_result(kb_workload_checked_root_file_open(path, 0, &fd));
   if (result != KB_WORKLOAD_OK)
      return result;
   size_t used = 0;
   for (;;)
   {
      unsigned char extra;
      unsigned char *target = used < cap ? out + used : &extra;
      size_t available = used < cap ? cap - used : 1;
      ssize_t n = read(fd, target, available);
      if (n > 0)
      {
         if (used == cap)
         {
            result = KB_WORKLOAD_INTEGRITY;
            break;
         }
         used += (size_t)n;
      }
      else if (n == 0)
      {
         result = used ? KB_WORKLOAD_OK : KB_WORKLOAD_INTEGRITY;
         break;
      }
      else if (errno != EINTR)
      {
         result = KB_WORKLOAD_UNAVAILABLE;
         break;
      }
   }
   close(fd);
   if (result == KB_WORKLOAD_OK)
      *out_len = used;
   else
      OPENSSL_cleanse(out, cap);
   return result;
}

static kb_workload_result_t validate_jwt(kb_workload_provider_t *provider,
                                         const kb_workload_wire_response_t *wire,
                                         kb_workload_identity_t *identity)
{
   unsigned char *jwks = malloc(WORKLOAD_JWKS_MAX);
   if (!jwks)
      return KB_WORKLOAD_UNAVAILABLE;
   kb_workload_result_t result = KB_WORKLOAD_UNAVAILABLE;
   time_t now = time(NULL);
   if (now > 0)
   {
      for (int attempt = 0; attempt < 2; ++attempt)
      {
         size_t jwks_len = 0;
         int reload_jwks = 0;
         OPENSSL_cleanse(jwks, WORKLOAD_JWKS_MAX);
         result = checked_file_read(provider->jwks_path, jwks, WORKLOAD_JWKS_MAX, &jwks_len);
         if (result != KB_WORKLOAD_OK)
            break;
         result = kb_workload_jwt_validate_ex(
             wire->token.ptr, wire->token.len, jwks, jwks_len, provider->expected_issuer,
             provider->expected_audience, (uint64_t)now, provider->max_token_age_seconds, identity,
             &reload_jwks);
         if (!reload_jwks || attempt == 1)
            break;
      }
   }
   OPENSSL_cleanse(jwks, WORKLOAD_JWKS_MAX);
   free(jwks);
   if (result == KB_WORKLOAD_INVALID)
      result = KB_WORKLOAD_INTEGRITY; /* helper/JWKS data is hostile, not caller input */
   return result;
}

static kb_workload_result_t provider_call(kb_workload_provider_t *provider,
                                          kb_workload_operation_t operation,
                                          const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                          const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                          const void *request_data, size_t request_data_len,
                                          kb_workload_identity_t *identity, unsigned char *output,
                                          size_t output_cap, size_t *output_len)
{
   size_t required_output_cap = operation == KB_WORKLOAD_OP_WRAP     ? KB_WORKLOAD_WIRE_CIPHER_MAX
                                : operation == KB_WORKLOAD_OP_UNWRAP ? KB_WORKLOAD_WIRE_PLAIN_MAX
                                                                     : 0;
   if (!provider || !challenge || !binding || !identity ||
       (operation != KB_WORKLOAD_OP_ATTEST && (!request_data || !request_data_len || !output ||
                                               !output_len || output_cap < required_output_cap)) ||
       (operation == KB_WORKLOAD_OP_ATTEST &&
        (request_data || request_data_len || output || output_cap || output_len)))
   {
      if (identity)
         OPENSSL_cleanse(identity, sizeof(*identity));
      if (output_len)
         *output_len = 0;
      if (output && output_cap)
         output[0] = 0;
      return KB_WORKLOAD_INVALID;
   }

   unsigned char *request = malloc(KB_WORKLOAD_WIRE_FRAME_MAX);
   unsigned char *response = malloc(KB_WORKLOAD_WIRE_FRAME_MAX);
   if (!request || !response)
   {
      OPENSSL_cleanse(identity, sizeof(*identity));
      if (output_len)
         *output_len = 0;
      if (output)
         OPENSSL_cleanse(output, required_output_cap);
      free(request);
      free(response);
      return KB_WORKLOAD_UNAVAILABLE;
   }
   size_t request_len = 0, response_len = 0;
   kb_workload_result_t result = KB_WORKLOAD_UNAVAILABLE;
   kb_workload_identity_t candidate;
   kb_workload_wire_response_t parsed;
   int old_cancel_state = 0, cancellation_disabled = 0;
   memset(&candidate, 0, sizeof(candidate));
   memset(&parsed, 0, sizeof(parsed));
   /* Capture all caller inputs before clearing any output. The API permits
    * in-place and partially overlapping wrap/unwrap buffers. */
   if (kb_workload_wire_build_request(operation, challenge, binding, request_data, request_data_len,
                                      request, KB_WORKLOAD_WIRE_FRAME_MAX, &request_len) != 0)
   {
      result = KB_WORKLOAD_INVALID;
      goto done;
   }
   OPENSSL_cleanse(identity, sizeof(*identity));
   if (output_len)
      *output_len = 0;
   if (output && output_cap)
      OPENSSL_cleanse(output, required_output_cap);

   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state) != 0)
      goto done;
   cancellation_disabled = 1;
   if (pthread_mutex_lock(&provider->mutex) != 0)
      goto done;
   int locked = 1;
   kb_workload_helper_result_t invoked = kb_workload_helper_invoke(
       provider->helper_fd, request, request_len, response, KB_WORKLOAD_WIRE_FRAME_MAX,
       &response_len, (int)provider->helper_timeout_ms);
   result = helper_result(invoked);
   if (result == KB_WORKLOAD_INVALID)
      result = KB_WORKLOAD_INTEGRITY;
   if (result != KB_WORKLOAD_OK)
      goto unlock;
   if (kb_workload_wire_parse_response(response, response_len, operation, &parsed) != 0)
   {
      result = KB_WORKLOAD_INTEGRITY;
      goto unlock;
   }
   result = parsed.status;
   if (result != KB_WORKLOAD_OK)
      goto unlock;
   result = validate_jwt(provider, &parsed, &candidate);
   if (result != KB_WORKLOAD_OK)
      goto unlock;
   if (CRYPTO_memcmp(provider->proof_anchor_id, parsed.proof_anchor_id.ptr,
                     KB_WORKLOAD_ANCHOR_LEN) != 0)
   {
      result = KB_WORKLOAD_INTEGRITY;
      goto unlock;
   }
   memcpy(candidate.proof_anchor_id, parsed.proof_anchor_id.ptr, KB_WORKLOAD_ANCHOR_LEN);
   memcpy(candidate.custody_anchor_id, parsed.custody_anchor_id.ptr, KB_WORKLOAD_ANCHOR_LEN);
   const unsigned char *captured_challenge = request + KB_WORKLOAD_WIRE_HEADER_LEN + 4;
   const unsigned char *captured_binding = captured_challenge + KB_WORKLOAD_CHALLENGE_LEN + 4;
   const unsigned char *captured_request_data = operation == KB_WORKLOAD_OP_ATTEST
                                                    ? NULL
                                                    : request + KB_WORKLOAD_WIRE_HEADER_LEN + 4 +
                                                          KB_WORKLOAD_CHALLENGE_LEN + 4 +
                                                          KB_WORKLOAD_BINDING_LEN + 4;
   if (kb_workload_proof_verify(provider->proof_key, operation, captured_challenge,
                                captured_binding, parsed.token.ptr, parsed.token.len,
                                parsed.proof_anchor_id.ptr, parsed.custody_anchor_id.ptr,
                                captured_request_data, request_data_len, parsed.data.ptr,
                                parsed.data.len, parsed.proof.ptr, parsed.proof.len) != 0)
   {
      result = KB_WORKLOAD_INTEGRITY;
      goto unlock;
   }
   if (operation != KB_WORKLOAD_OP_ATTEST)
   {
      if (parsed.data.len > output_cap)
      {
         result = KB_WORKLOAD_INTEGRITY;
         goto unlock;
      }
      memcpy(output, parsed.data.ptr, parsed.data.len);
      *output_len = parsed.data.len;
   }
   *identity = candidate;
   result = KB_WORKLOAD_OK;
unlock:
   if (locked)
      (void)pthread_mutex_unlock(&provider->mutex);
done:
   if (result != KB_WORKLOAD_OK)
   {
      OPENSSL_cleanse(identity, sizeof(*identity));
      if (output && output_cap)
         OPENSSL_cleanse(output, required_output_cap);
      if (output_len)
         *output_len = 0;
   }
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(&parsed, sizeof(parsed));
   OPENSSL_cleanse(request, KB_WORKLOAD_WIRE_FRAME_MAX);
   OPENSSL_cleanse(response, KB_WORKLOAD_WIRE_FRAME_MAX);
   free(request);
   free(response);
   if (cancellation_disabled)
   {
      /* Restore the caller's state first, then deliver any cancellation deferred
       * while the provider mutex and helper lifecycle were protected. */
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      pthread_testcancel();
   }
   return result;
}

kb_workload_result_t kb_workload_provider_open(const kb_workload_provider_config_t *config,
                                               kb_workload_provider_t **out)
{
   if (out)
      *out = NULL;
   if (!config || !out)
      return KB_WORKLOAD_INVALID;
   if (config->kind == KB_WORKLOAD_PROVIDER_NONE || config->kind == KB_WORKLOAD_PROVIDER_TPM2_V1 ||
       config->kind == KB_WORKLOAD_PROVIDER_PKCS11_V1)
      return KB_WORKLOAD_DISABLED;
   if (config->kind != KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 || !absolute_path(config->helper_path) ||
       !absolute_path(config->jwks_path) || !absolute_path(config->proof_spki_path) ||
       !printable(config->expected_issuer, 600) || !printable(config->expected_audience, 600) ||
       !config->max_token_age_seconds || config->max_token_age_seconds > 300 ||
       !config->helper_timeout_ms || config->helper_timeout_ms > KB_WORKLOAD_HELPER_TIMEOUT_MAX_MS)
      return KB_WORKLOAD_INVALID;

   kb_workload_provider_t *provider = calloc(1, sizeof(*provider));
   if (!provider)
      return KB_WORKLOAD_UNAVAILABLE;
   provider->helper_fd = -1;
   provider->kind = config->kind;
   memcpy(provider->helper_path, config->helper_path, strlen(config->helper_path) + 1);
   memcpy(provider->jwks_path, config->jwks_path, strlen(config->jwks_path) + 1);
   memcpy(provider->proof_spki_path, config->proof_spki_path, strlen(config->proof_spki_path) + 1);
   memcpy(provider->expected_issuer, config->expected_issuer, strlen(config->expected_issuer) + 1);
   memcpy(provider->expected_audience, config->expected_audience,
          strlen(config->expected_audience) + 1);
   provider->max_token_age_seconds = config->max_token_age_seconds;
   provider->helper_timeout_ms = config->helper_timeout_ms;
   kb_workload_result_t result =
       helper_result(kb_workload_helper_open(provider->helper_path, &provider->helper_fd));
   if (result != KB_WORKLOAD_OK)
      goto fail;

   unsigned char spki[KB_WORKLOAD_PROOF_SPKI_MAX];
   unsigned char jwks_probe[WORKLOAD_JWKS_MAX];
   size_t spki_len = 0, jwks_len = 0;
   result = checked_file_read(provider->proof_spki_path, spki, sizeof(spki), &spki_len);
   if (result == KB_WORKLOAD_OK &&
       kb_workload_proof_key_load_der(spki, spki_len, &provider->proof_key) != 0)
      result = KB_WORKLOAD_INTEGRITY;
   if (result == KB_WORKLOAD_OK &&
       kb_workload_proof_anchor_id(provider->proof_key, provider->proof_anchor_id) != 0)
      result = KB_WORKLOAD_INTEGRITY;
   if (result == KB_WORKLOAD_OK)
      result = checked_file_read(provider->jwks_path, jwks_probe, sizeof(jwks_probe), &jwks_len);
   OPENSSL_cleanse(spki, sizeof(spki));
   OPENSSL_cleanse(jwks_probe, sizeof(jwks_probe));
   if (result != KB_WORKLOAD_OK)
      goto fail;
   if (pthread_mutex_init(&provider->mutex, NULL) != 0)
   {
      result = KB_WORKLOAD_UNAVAILABLE;
      goto fail;
   }
   provider->mutex_ready = 1;
   *out = provider;
   return KB_WORKLOAD_OK;

fail:
   kb_workload_provider_close(provider);
   return result;
}

kb_workload_result_t kb_workload_attest(kb_workload_provider_t *provider,
                                        const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                        const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                        kb_workload_identity_t *identity)
{
   return provider_call(provider, KB_WORKLOAD_OP_ATTEST, challenge, binding, NULL, 0, identity,
                        NULL, 0, NULL);
}

kb_workload_result_t kb_workload_wrap(kb_workload_provider_t *provider,
                                      const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                      const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                      const void *plain, size_t plain_len,
                                      kb_workload_identity_t *identity, unsigned char *cipher,
                                      size_t cap, size_t *len)
{
   return provider_call(provider, KB_WORKLOAD_OP_WRAP, challenge, binding, plain, plain_len,
                        identity, cipher, cap, len);
}

kb_workload_result_t kb_workload_unwrap(kb_workload_provider_t *provider,
                                        const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                        const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                        const void *cipher, size_t cipher_len,
                                        kb_workload_identity_t *identity, unsigned char *plain,
                                        size_t cap, size_t *len)
{
   return provider_call(provider, KB_WORKLOAD_OP_UNWRAP, challenge, binding, cipher, cipher_len,
                        identity, plain, cap, len);
}

void kb_workload_provider_close(kb_workload_provider_t *provider)
{
   if (!provider)
      return;
   if (provider->helper_fd >= 0)
      close(provider->helper_fd);
   kb_workload_proof_key_close(provider->proof_key);
   if (provider->mutex_ready)
      pthread_mutex_destroy(&provider->mutex);
   OPENSSL_cleanse(provider, sizeof(*provider));
   free(provider);
}
