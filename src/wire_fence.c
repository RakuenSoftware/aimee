#include <stdlib.h>
#include <stdio.h>
#include "http_retry.h"
#include "agent_exec.h"
#include <openssl/evp.h>
#include <string.h>
#include "wire_fence.h"
#include "request_context.h"
#include "modules/economizer/economizer_module_client.h"

/* Lean clients may have no HTTP context. Deployment limits still apply, and a
 * present limit must never bypass admission because the module is absent. */
extern const request_context_t *request_context_get(void) __attribute__((weak));
extern int ingress_preinject_revalidate_sources(void) __attribute__((weak));
extern econ_request_budget_result_t econ_module_request_budget(unsigned, const void *, size_t,
                                                               const char *) __attribute__((weak));
extern econ_request_budget_result_t
econ_module_request_budget_with_policy(unsigned, const void *, size_t, const char *, const char *)
    __attribute__((weak));
extern int server_error_kind_http_status(const char *) __attribute__((weak));
static _Thread_local const char *last_error;
const char *wire_fence_last_error(void)
{
   return last_error ? last_error : "request_pipeline_unavailable";
}
int wire_fence_error_http_status(const char *error)
{
   if (!error)
      return 502;
   if (!strcmp(error, "request_budget_exceeded"))
      return 413;
   if (!strcmp(error, "request_budget_invalid") || !strcmp(error, "token_count_unavailable"))
      return 400;
   if (!strcmp(error, "request_budget_unavailable") ||
       !strcmp(error, "request_budget_policy_invalid"))
      return 503;
   int classified = server_error_kind_http_status ? server_error_kind_http_status(error) : 0;
   return classified ? classified : 502;
}
const char *wire_fence_error_type(const char *error)
{
   return wire_fence_error_http_status(error) == 502 ? "upstream_error" : error;
}

struct wire_fence
{
   wire_fence_route_t route;
   uint8_t *data;
   size_t len;
};

static int route_valid(wire_fence_route_t route)
{
   return route == WIRE_FENCE_OPENAI_CHAT || route == WIRE_FENCE_OPENAI_RESPONSES ||
          route == WIRE_FENCE_ANTHROPIC_MESSAGES;
}

/* THE FENCE HAS NO CANDIDATE PATH, and that is what makes it safe.
 *
 * This used to also assert that the proof registry was signed and EMPTY, i.e.
 * that no transform was authorized to alter the wire body. That assertion moved
 * with the proof planner into the Go economizer module, and re-asking it here
 * would mean a bus round trip on every provider call to learn something this
 * file already guarantees structurally: wire_fence_select only ever returns the
 * PRISTINE bytes, either passed through or frozen into a snapshot. There is no
 * branch that can emit anything else.
 *
 * IF YOU ADD A CANDIDATE PATH HERE, that guarantee is gone and you must gate it
 * on an authorization decision from the economizer module. Do not add one and
 * assume this fence still protects you. */
int wire_fence_create(wire_fence_route_t route, const void *pristine, size_t pristine_len,
                      wire_fence_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!route_valid(route) || (!pristine && pristine_len != 0))
      return -1;
   if (pristine_len == SIZE_MAX)
      return -1;

   wire_fence_t *snapshot = calloc(1, sizeof(*snapshot));
   if (!snapshot)
      return -1;
   snapshot->data = malloc(pristine_len + 1);
   if (!snapshot->data)
   {
      free(snapshot);
      return -1;
   }
   if (pristine_len)
      memcpy(snapshot->data, pristine, pristine_len);
   snapshot->data[pristine_len] = '\0';
   snapshot->route = route;
   snapshot->len = pristine_len;
   *out = snapshot;
   return 0;
}

int wire_fence_revalidate_sources(void)
{
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   if (context && (context->context_refused || (context->memory_source_release[0] &&
                                                (!ingress_preinject_revalidate_sources ||
                                                 ingress_preinject_revalidate_sources() != 0))))
   {
      last_error = context->context_refusal_kind[0] ? context->context_refusal_kind : "unavailable";
      return WIRE_FENCE_CONTEXT_REFUSED;
   }
   return 0;
}

int wire_fence_external_backend(void)
{
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   if (context && context->context_refused)
   {
      last_error = context->context_refusal_kind[0] ? context->context_refusal_kind : "unavailable";
      return WIRE_FENCE_CONTEXT_REFUSED;
   }
   const char *policy = getenv("AIMEE_PROVIDER_CONTEXT_LIMITS");
   if ((policy && *policy) || (context && context->request_budget_present))
   {
      last_error = "request_budget_unavailable";
      return -1;
   }
   return 0;
}

int wire_fence_select(int proof_gated, wire_fence_route_t route, const void *pristine,
                      size_t pristine_len, wire_fence_t **snapshot, wire_fence_bytes_t *selected)
{
   last_error = "request_pipeline_unavailable";
   if (!snapshot || !selected || (!pristine && pristine_len != 0) || !route_valid(route))
      return -1;
   *snapshot = NULL;
   selected->data = NULL;
   selected->len = 0;
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   if (context && context->context_refused)
   {
      last_error = context->context_refusal_kind[0] ? context->context_refusal_kind : "unavailable";
      return WIRE_FENCE_CONTEXT_REFUSED;
   }
   /* Deployment-owned metadata is forwarded unchanged. Go owns validation and
    * intersection with the caller's limit. Empty/unset means no operator cap;
    * literal zero is expressed in the versioned JSON object. */
   const char *policy = getenv("AIMEE_PROVIDER_CONTEXT_LIMITS");
   if (policy && !*policy)
      policy = NULL;
   if (policy || (context && context->request_budget_present))
   {
      const char *limits =
          context && context->request_budget_present > 0 ? context->request_budget_limits : NULL;
      econ_request_budget_result_t result =
          context && context->request_budget_present < 0 ? ECON_REQUEST_BUDGET_INVALID
          : policy                                       ? (econ_module_request_budget_with_policy
                                                                ? econ_module_request_budget_with_policy((unsigned)route, pristine,
                                                                                                         pristine_len, limits, policy)
                                                                : ECON_REQUEST_BUDGET_UNAVAILABLE)
          : econ_module_request_budget
              ? econ_module_request_budget((unsigned)route, pristine, pristine_len, limits)
              : ECON_REQUEST_BUDGET_UNAVAILABLE;
      if (result != ECON_REQUEST_BUDGET_ADMITTED)
      {
         last_error = result == ECON_REQUEST_BUDGET_INVALID              ? "request_budget_invalid"
                      : result == ECON_REQUEST_BUDGET_OVERFLOW           ? "request_budget_exceeded"
                      : result == ECON_REQUEST_BUDGET_TOKENS_UNAVAILABLE ? "token_count_unavailable"
                      : result == ECON_REQUEST_BUDGET_POLICY_INVALID
                          ? "request_budget_policy_invalid"
                          : "request_budget_unavailable";
         return -1;
      }
   }
   if (wire_fence_revalidate_sources() != 0)
      return WIRE_FENCE_CONTEXT_REFUSED;
   if (!proof_gated)
   {
      selected->data = pristine;
      selected->len = pristine_len;
      return 0;
   }
   if (wire_fence_create(route, pristine, pristine_len, snapshot) != 0)
      return -1;
   *selected = wire_fence_bytes(*snapshot);
   return 0;
}

wire_fence_route_t econ_wire_snapshot_route(const wire_fence_t *snapshot)
{
   return snapshot ? snapshot->route : 0;
}

wire_fence_bytes_t wire_fence_bytes(const wire_fence_t *snapshot)
{
   wire_fence_bytes_t bytes = {0};
   if (snapshot)
   {
      bytes.data = snapshot->data;
      bytes.len = snapshot->len;
   }
   return bytes;
}

void wire_fence_destroy(wire_fence_t *snapshot)
{
   if (!snapshot)
      return;
   free(snapshot->data);
   snapshot->data = NULL;
   snapshot->len = 0;
   free(snapshot);
}

/* This state belongs to one synchronous call, including its retries. Nested
 * owner HTTP requests use the ordinary transport and cannot inherit it. */
extern int ingress_preinject_prepare_attempt(const void *, size_t, const char *, const char *,
                                             const char *, char[33]) __attribute__((weak));
extern int ingress_preinject_observe_attempt(const char *, int, const char *, size_t)
    __attribute__((weak));
typedef struct
{
   wire_fence_route_t route;
   const char *provider, *model;
   char attempt[33];
   void *send_guard_state;
} wire_attempt_t;

extern int ingress_preinject_acquire_send_guard(void **) __attribute__((weak));
extern void ingress_preinject_release_send_guard(void *) __attribute__((weak));
static int wire_send_acquire(void *opaque)
{
   wire_attempt_t *attempt = opaque;
   if (!ingress_preinject_acquire_send_guard || !ingress_preinject_release_send_guard)
      return -1;
   return ingress_preinject_acquire_send_guard(&attempt->send_guard_state);
}
static void wire_send_release(void *opaque, int status)
{
   (void)status;
   wire_attempt_t *attempt = opaque;
   if (ingress_preinject_release_send_guard)
      ingress_preinject_release_send_guard(attempt->send_guard_state);
   attempt->send_guard_state = NULL;
}
static int wire_send_guard_required(void)
{
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   return context && context->memory_source_release[0];
}

static int wire_attempt_before(void *opaque, const void *body, size_t length)
{
   wire_attempt_t *attempt = opaque;
   attempt->attempt[0] = '\0';
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   if (!context)
      return 0;
   if (context->context_refused)
      goto refused;
   if (!context->memory_source_release[0] && !context->memory_receipt_required)
      return 0;
   const char *route = attempt->route == WIRE_FENCE_OPENAI_CHAT          ? "openai_chat"
                       : attempt->route == WIRE_FENCE_OPENAI_RESPONSES   ? "openai_responses"
                       : attempt->route == WIRE_FENCE_ANTHROPIC_MESSAGES ? "anthropic_messages"
                                                                         : "";
   if (ingress_preinject_prepare_attempt &&
       ingress_preinject_prepare_attempt(body, length, route, attempt->provider, attempt->model,
                                         attempt->attempt) == 0)
      return 0;
refused:
   last_error = context->context_refusal_kind[0] ? context->context_refusal_kind : "unavailable";
   return -1;
}

static void wire_attempt_after(void *opaque, int status, const char *response, size_t length)
{
   wire_attempt_t *attempt = opaque;
   if (attempt->attempt[0] &&
       (!ingress_preinject_observe_attempt ||
        ingress_preinject_observe_attempt(attempt->attempt, status, response, length) != 0))
      fputs(
          "provider receipt observation unavailable; admitted attempt outcome remains unresolved\n",
          stderr);
}

int wire_fence_post(const char *url, const char *auth_header, const void *body, size_t body_len,
                    char **response_buf, int timeout_ms, const char *extra_headers,
                    int max_attempts, int base_ms, int max_ms, const char *provider,
                    const char *model, const char *session_id, wire_fence_route_t route)
{
   wire_attempt_t attempt = {.route = route, .provider = provider, .model = model};
   agent_http_send_guard_t guard = {.context = &attempt,
                                    .acquire = wire_send_acquire,
                                    .release = wire_send_release,
                                    .send_timeout_ms = 4500};
   http_retry_observer_t observer = {.context = &attempt,
                                     .before = wire_attempt_before,
                                     .after = wire_attempt_after,
                                     .send_guard = wire_send_guard_required() ? &guard : NULL};
   return http_retry_post_observed_bytes(url, auth_header, body, body_len, response_buf, timeout_ms,
                                         extra_headers, max_attempts, base_ms, max_ms, provider,
                                         model, session_id, NULL, &observer);
}

extern int ingress_preinject_observe_commitment(const char *, int, const char *, size_t,
                                                const char *) __attribute__((weak));
typedef struct
{
   EVP_MD_CTX *digest;
   size_t length;
   wire_fence_stream_cb callback;
   void *context;
} wire_stream_t;

static int wire_stream_chunk(const char *data, size_t length, void *opaque)
{
   wire_stream_t *stream = opaque;
   if (length > SIZE_MAX - stream->length || EVP_DigestUpdate(stream->digest, data, length) != 1)
      return -1;
   stream->length += length;
   return stream->callback ? stream->callback(data, length, stream->context) : 0;
}

int wire_fence_post_stream(const char *url, const char *auth_header, const void *body,
                           size_t body_len, wire_fence_stream_cb callback, void *userdata,
                           int timeout_ms, const char *extra_headers, const char *provider,
                           const char *model, wire_fence_route_t route)
{
   wire_attempt_t attempt = {.route = route, .provider = provider, .model = model};
   if (wire_attempt_before(&attempt, body, body_len) != 0)
      return HTTP_RETRY_ADMISSION_REFUSED;
   if (!attempt.attempt[0])
      return agent_http_post_stream_bytes(url, auth_header, body, body_len, callback, userdata,
                                          timeout_ms, extra_headers);
   wire_stream_t stream = {.digest = EVP_MD_CTX_new(), .callback = callback, .context = userdata};
   if (!stream.digest || EVP_DigestInit_ex(stream.digest, EVP_sha256(), NULL) != 1)
   {
      EVP_MD_CTX_free(stream.digest);
      wire_attempt_after(&attempt, -1, NULL, 0);
      last_error = "unavailable";
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   agent_http_send_guard_t guard = {.context = &attempt,
                                    .acquire = wire_send_acquire,
                                    .release = wire_send_release,
                                    .send_timeout_ms = 4500};
   int status =
       wire_send_guard_required()
           ? agent_http_post_stream_guarded_bytes(url, auth_header, body, body_len,
                                                  wire_stream_chunk, &stream, timeout_ms,
                                                  extra_headers, &guard)
           : agent_http_post_stream_bytes(url, auth_header, body, body_len, wire_stream_chunk,
                                          &stream, timeout_ms, extra_headers);
   unsigned char raw[32];
   unsigned int length = 0;
   char digest[65];
   int final = EVP_DigestFinal_ex(stream.digest, raw, &length);
   EVP_MD_CTX_free(stream.digest);
   if (final == 1 && length == 32)
   {
      static const char hex[] = "0123456789abcdef";
      for (unsigned i = 0; i < 32; i++)
      {
         digest[2 * i] = hex[raw[i] >> 4];
         digest[2 * i + 1] = hex[raw[i] & 15];
      }
      digest[64] = '\0';
      if (ingress_preinject_observe_commitment &&
          ingress_preinject_observe_commitment(attempt.attempt, status, digest, stream.length,
                                               "provider_stream_bytes") == 0)
         return status;
   }
   fputs("provider stream receipt observation unavailable; admitted attempt outcome remains "
         "unresolved\n",
         stderr);
   return status;
}
