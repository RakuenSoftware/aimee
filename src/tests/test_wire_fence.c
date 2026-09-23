#include "wire_fence.h"
#include "http_retry.h"
#include "aimee_sha256.h"
#include "request_context.h"
#include "modules/economizer/economizer_module_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recorded classifications supplied through the same Go-provider seam used by
 * the server; the wire fence must not invent memory-specific HTTP policy. */
int server_error_kind_http_status(const char *kind)
{
   if (!strcmp(kind, "protected_context_overflow"))
      return 413;
   if (!strcmp(kind, "unavailable"))
      return 503;
   return 0;
}

static request_context_t context;
static econ_request_budget_result_t admission;
static int admission_calls;
static int have_context = 1;
const request_context_t *request_context_get(void)
{
   return have_context ? &context : NULL;
}
econ_request_budget_result_t econ_module_request_budget(unsigned route, const void *body,
                                                        size_t length, const char *limits)
{
   assert(route >= 1 && route <= 3);
   assert(length == 3 && memcmp(body, "abc", 3) == 0);
   assert(strcmp(limits, "limits") == 0);
   admission_calls++;
   return admission;
}

econ_request_budget_result_t econ_module_request_budget_with_policy(unsigned route,
                                                                    const void *body, size_t length,
                                                                    const char *limits,
                                                                    const char *policy)
{
   assert(route >= 1 && route <= 3);
   assert(length == 3 && memcmp(body, "abc", 3) == 0);
   assert(strcmp(policy, "opaque operator policy") == 0);
   assert(limits == NULL || strcmp(limits, "limits") == 0);
   admission_calls++;
   return admission;
}

static void operator_policy(const char *value)
{
#ifdef _WIN32
   assert(_putenv_s("AIMEE_PROVIDER_CONTEXT_LIMITS", value ? value : "") == 0);
#else
   assert((value ? setenv("AIMEE_PROVIDER_CONTEXT_LIMITS", value, 1)
                 : unsetenv("AIMEE_PROVIDER_CONTEXT_LIMITS")) == 0);
#endif
}

static void test_hard_budget_refuses_without_selected_bytes(void)
{
   const char *errors[] = {NULL,
                           "request_budget_invalid",
                           "request_budget_exceeded",
                           "token_count_unavailable",
                           "request_budget_unavailable",
                           "request_budget_policy_invalid"};
   const int statuses[] = {0, 400, 413, 400, 503, 503};
   context.request_budget_present = 1;
   strcpy(context.request_budget_limits, "limits");
   for (int gated = 0; gated <= 1; gated++)
      for (unsigned route = 1; route <= 3; route++)
         for (int result = 0; result <= 5; result++)
         {
            admission = (econ_request_budget_result_t)result;
            wire_fence_t *snapshot = NULL;
            wire_fence_bytes_t selected = {0};
            int rc =
                wire_fence_select(gated, (wire_fence_route_t)route, "abc", 3, &snapshot, &selected);
            if (result == 0)
            {
               assert(rc == 0 && selected.len == 3 && memcmp(selected.data, "abc", 3) == 0);
               wire_fence_destroy(snapshot);
            }
            else
            {
               assert(rc == -1 && snapshot == NULL && selected.data == NULL && selected.len == 0);
               assert(strcmp(wire_fence_last_error(), errors[result]) == 0);
               assert(wire_fence_error_http_status(wire_fence_last_error()) == statuses[result]);
               assert(strcmp(wire_fence_error_type(wire_fence_last_error()), errors[result]) == 0);
            }
         }
   assert(admission_calls == 36);
   context.request_budget_present = -1;
   wire_fence_t *snapshot = NULL;
   wire_fence_bytes_t selected = {0};
   assert(wire_fence_select(0, WIRE_FENCE_OPENAI_CHAT, "abc", 3, &snapshot, &selected) == -1);
   assert(admission_calls == 36);
   assert(strcmp(wire_fence_last_error(), "request_budget_invalid") == 0);
   memset(&context, 0, sizeof(context));
   assert(wire_fence_select(0, WIRE_FENCE_OPENAI_CHAT, "abc", 3, &snapshot, &selected) == 0);
   assert(admission_calls == 36);
}

static void test_operator_policy_reaches_admission_without_request_header(void)
{
   operator_policy("opaque operator policy");
   for (have_context = 0; have_context <= 1; have_context++)
      for (int caller = 0; caller <= 1; caller++)
         for (int gated = 0; gated <= 1; gated++)
            for (unsigned route = 1; route <= 3; route++)
            {
               context.request_budget_present = caller;
               strcpy(context.request_budget_limits, "limits");
               wire_fence_t *snapshot = NULL;
               wire_fence_bytes_t selected = {0};
               int before = admission_calls;
               admission = ECON_REQUEST_BUDGET_OVERFLOW;
               assert(wire_fence_select(gated, (wire_fence_route_t)route, "abc", 3, &snapshot,
                                        &selected) == -1);
               assert(admission_calls == before + 1 && snapshot == NULL && selected.data == NULL);
               assert(strcmp(wire_fence_last_error(), "request_budget_exceeded") == 0);
            }
   have_context = 1;
   memset(&context, 0, sizeof(context));
   operator_policy(NULL);
}

static void test_pristine_copy_is_immutable(void)
{
   char source[] = "{\"model\":\"gpt-5.6\"}";
   size_t len = strlen(source);
   wire_fence_t *snapshot = NULL;
   assert(wire_fence_create(WIRE_FENCE_OPENAI_RESPONSES, source, len, &snapshot) == 0);
   memset(source, 'x', len);
   wire_fence_bytes_t bytes = wire_fence_bytes(snapshot);
   assert(bytes.len == len);
   assert(memcmp(bytes.data, "{\"model\":\"gpt-5.6\"}", len) == 0);
   assert(econ_wire_snapshot_route(snapshot) == WIRE_FENCE_OPENAI_RESPONSES);
   wire_fence_destroy(snapshot);
}

static void test_explicit_length_preserves_embedded_nul(void)
{
   const unsigned char source[] = {'a', 0, 'b'};
   wire_fence_t *snapshot = NULL;
   assert(wire_fence_create(WIRE_FENCE_ANTHROPIC_MESSAGES, source, sizeof(source), &snapshot) == 0);
   wire_fence_bytes_t bytes = wire_fence_bytes(snapshot);
   assert(bytes.len == sizeof(source));
   assert(memcmp(bytes.data, source, sizeof(source)) == 0);
   wire_fence_destroy(snapshot);
}

static void test_invalid_inputs_fail_without_snapshot(void)
{
   wire_fence_t *snapshot = (wire_fence_t *)1;
   assert(wire_fence_create(0, "{}", 2, &snapshot) == -1);
   assert(snapshot == NULL);
   snapshot = (wire_fence_t *)1;
   assert(wire_fence_create(WIRE_FENCE_OPENAI_CHAT, NULL, 1, &snapshot) == -1);
   assert(snapshot == NULL);
   assert(wire_fence_create(WIRE_FENCE_OPENAI_CHAT, "{}", 2, NULL) == -1);
   wire_fence_destroy(NULL);
}

static void test_off_bypasses_snapshot(void)
{
   const char body[] = "{}";
   wire_fence_t *snapshot = (wire_fence_t *)1;
   wire_fence_bytes_t selected = {0};
   assert(wire_fence_select(0, WIRE_FENCE_OPENAI_CHAT, body, 2, &snapshot, &selected) == 0);
   assert(snapshot == NULL);
   assert(selected.data == (const uint8_t *)body);
   assert(selected.len == 2);
}

static void test_proof_gated_empty_registry_is_byte_identical_on_every_route(void)
{
   const unsigned char body[] = {'{', ' ', '"', 'x', '"', ':', ' ', '1', ' ', '}'};
   const wire_fence_route_t routes[] = {WIRE_FENCE_OPENAI_CHAT, WIRE_FENCE_OPENAI_RESPONSES,
                                        WIRE_FENCE_ANTHROPIC_MESSAGES};
   for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
   {
      wire_fence_t *snapshot = NULL;
      wire_fence_bytes_t selected = {0};
      assert(wire_fence_select(1, routes[i], body, sizeof(body), &snapshot, &selected) == 0);
      assert(snapshot != NULL);
      assert(selected.len == sizeof(body));
      assert(memcmp(selected.data, body, sizeof(body)) == 0);
      assert(selected.data != body);
      assert(econ_wire_snapshot_route(snapshot) == routes[i]);
      wire_fence_destroy(snapshot);
   }
}

static void test_context_refusal_blocks_every_route(void)
{
   memset(&context, 0, sizeof(context));
   have_context = 1;
   context.context_refused = 1;
   strcpy(context.context_refusal_kind, "protected_context_overflow");
   int before = admission_calls;
   for (int gated = 0; gated < 2; gated++)
      for (unsigned route = 1; route <= 3; route++)
      {
         wire_fence_t *snapshot = NULL;
         wire_fence_bytes_t selected = {0};
         assert(wire_fence_select(gated, (wire_fence_route_t)route, "abc", 3, &snapshot,
                                  &selected) == WIRE_FENCE_CONTEXT_REFUSED);
         assert(!snapshot && !selected.data && !selected.len);
         assert(strcmp(wire_fence_last_error(), "protected_context_overflow") == 0);
         assert(wire_fence_error_http_status(wire_fence_last_error()) == 413);
         assert(strcmp(wire_fence_error_type(wire_fence_last_error()),
                       "protected_context_overflow") == 0);
      }
   assert(admission_calls == before);
   memset(&context, 0, sizeof(context));
   wire_fence_t *snapshot = NULL;
   wire_fence_bytes_t selected = {0};
   assert(wire_fence_select(0, WIRE_FENCE_OPENAI_CHAT, "abc", 3, &snapshot, &selected) == 0);
   assert(selected.len == 3);
}
static void test_source_handle_requires_host_transport(void)
{
   memset(&context, 0, sizeof(context));
   strcpy(context.memory_source_release, "opaque-source-handle");
   for (int gated = 0; gated < 2; gated++)
      for (unsigned route = 1; route <= 3; route++)
      {
         wire_fence_t *snapshot = NULL;
         wire_fence_bytes_t selected = {0};
         assert(wire_fence_select(gated, (wire_fence_route_t)route, "abc", 3, &snapshot,
                                  &selected) == WIRE_FENCE_CONTEXT_REFUSED);
         assert(!snapshot && !selected.data && !selected.len);
         assert(strcmp(wire_fence_last_error(), "unavailable") == 0);
      }
   memset(&context, 0, sizeof(context));
}

static int stream_before_calls, stream_after_calls, stream_transport_calls;
static int stream_refuse, stream_abort, stream_after_failure;
static int stream_observed_status;
static size_t stream_consumed, stream_observed_bytes;
static char stream_observed_digest[65];
static const unsigned char stream_bytes[] = {'a', 0, 'b', 0xce, 0xb1};

int ingress_preinject_prepare_attempt(const void *body, size_t length, const char *route,
                                      const char *provider, const char *model, char attempt[33])
{
   assert(body && length == 3 && memcmp(body, "abc", 3) == 0);
   assert(!strcmp(provider, "fixture") && !strcmp(model, "fixture-model"));
   assert(!strcmp(route, "openai_chat") || !strcmp(route, "openai_responses") ||
          !strcmp(route, "anthropic_messages"));
   stream_before_calls++;
   if (stream_refuse)
      return -1;
   strcpy(attempt, "0123456789abcdef0123456789abcdef");
   return 0;
}
int ingress_preinject_observe_commitment(const char *attempt, int status, const char *digest,
                                         size_t length, const char *representation)
{
   assert(!strcmp(attempt, "0123456789abcdef0123456789abcdef"));
   assert(!strcmp(representation, "provider_stream_bytes"));
   assert(strlen(digest) == 64);
   stream_after_calls++;
   stream_observed_status = status;
   stream_observed_bytes = length;
   strcpy(stream_observed_digest, digest);
   return stream_after_failure;
}
int agent_http_post_stream_bytes(const char *url, const char *auth, const void *body,
                                 size_t body_len, wire_fence_stream_cb callback, void *userdata,
                                 int timeout, const char *extra)
{
   assert(body_len == 3 && memcmp(body, "abc", 3) == 0);
   stream_transport_calls++;
   if (callback((const char *)stream_bytes, 3, userdata) != 0)
      return -1;
   if (callback((const char *)stream_bytes + 3, 2, userdata) != 0)
      return -1;
   return 200;
}
static int consume_stream(const char *bytes, size_t length, void *unused)
{
   assert(!memcmp(bytes, stream_bytes + stream_consumed, length));
   stream_consumed += length;
   return stream_abort;
}
static void test_stream_receipt_admission_and_commitment(void)
{
   for (int route = 1; route <= 3; route++)
      for (int scenario = 0; scenario < 7; scenario++)
      {
         memset(&context, 0, sizeof(context));
         have_context = scenario != 5;
         context.memory_receipt_required = scenario == 4;
         if (scenario < 4)
            strcpy(context.memory_source_release, "0123456789abcdef0123456789abcdef");
         stream_refuse = scenario == 1;
         stream_abort = scenario == 2;
         stream_after_failure = scenario == 3 ? -1 : 0;
         stream_before_calls = stream_after_calls = stream_transport_calls = 0;
         stream_consumed = stream_observed_bytes = 0;
         int status = wire_fence_post_stream("fixture", "secret-header", "abc", 3, consume_stream,
                                             NULL, 1000, "secret-extra", "fixture", "fixture-model",
                                             (wire_fence_route_t)route);
         if (stream_refuse)
         {
            assert(status == HTTP_RETRY_ADMISSION_REFUSED && stream_before_calls == 1);
            assert(!stream_transport_calls && !stream_after_calls && !stream_consumed);
            continue;
         }
         assert(status == (stream_abort ? -1 : 200) && stream_transport_calls == 1);
         assert(stream_consumed == (stream_abort ? 3 : sizeof(stream_bytes)));
         if (scenario >= 5)
            assert(stream_before_calls == 0 && stream_after_calls == 0);
         else
         {
            assert(stream_before_calls == 1 && stream_after_calls == 1);
            assert(stream_observed_status == status && stream_observed_bytes == stream_consumed);
            char digest[65];
            assert(aimee_sha256_hex(stream_bytes, stream_consumed, digest) == 0);
            assert(!strcmp(digest, stream_observed_digest));
         }
      }
   memset(&context, 0, sizeof(context));
   have_context = 1;
   puts("stream admission and exact incremental commitments survive partial/failed observations");
}

int main(void)
{
   test_stream_receipt_admission_and_commitment();
   operator_policy(NULL);
   test_source_handle_requires_host_transport();
   test_context_refusal_blocks_every_route();
   test_hard_budget_refuses_without_selected_bytes();
   test_operator_policy_reaches_admission_without_request_header();
   test_pristine_copy_is_immutable();
   test_explicit_length_preserves_embedded_nul();
   test_invalid_inputs_fail_without_snapshot();
   test_off_bypasses_snapshot();
   test_proof_gated_empty_registry_is_byte_identical_on_every_route();
   puts("economizer_wire_snapshot: ALL PASS");
   return 0;
}
