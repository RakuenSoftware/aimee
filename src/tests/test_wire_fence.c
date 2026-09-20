#include "wire_fence.h"
#include "request_context.h"
#include "modules/economizer/economizer_module_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static request_context_t context;
static econ_request_budget_result_t admission;
static int admission_calls;
const request_context_t *request_context_get(void)
{
   return &context;
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

static void test_hard_budget_refuses_without_selected_bytes(void)
{
   const char *errors[] = {NULL, "request_budget_invalid", "request_budget_exceeded",
                           "token_count_unavailable", "request_budget_unavailable"};
   const int statuses[] = {0, 400, 413, 400, 503};
   context.request_budget_present = 1;
   strcpy(context.request_budget_limits, "limits");
   for (int gated = 0; gated <= 1; gated++)
      for (unsigned route = 1; route <= 3; route++)
         for (int result = 0; result <= 4; result++)
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
   assert(admission_calls == 30);
   context.request_budget_present = -1;
   wire_fence_t *snapshot = NULL;
   wire_fence_bytes_t selected = {0};
   assert(wire_fence_select(0, WIRE_FENCE_OPENAI_CHAT, "abc", 3, &snapshot, &selected) == -1);
   assert(admission_calls == 30);
   assert(strcmp(wire_fence_last_error(), "request_budget_invalid") == 0);
   memset(&context, 0, sizeof(context));
   assert(wire_fence_select(0, WIRE_FENCE_OPENAI_CHAT, "abc", 3, &snapshot, &selected) == 0);
   assert(admission_calls == 30);
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

int main(void)
{
   test_hard_budget_refuses_without_selected_bytes();
   test_pristine_copy_is_immutable();
   test_explicit_length_preserves_embedded_nul();
   test_invalid_inputs_fail_without_snapshot();
   test_off_bypasses_snapshot();
   test_proof_gated_empty_registry_is_byte_identical_on_every_route();
   puts("economizer_wire_snapshot: ALL PASS");
   return 0;
}
