#include "wire_fence.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
   test_pristine_copy_is_immutable();
   test_explicit_length_preserves_embedded_nul();
   test_invalid_inputs_fail_without_snapshot();
   test_off_bypasses_snapshot();
   test_proof_gated_empty_registry_is_byte_identical_on_every_route();
   puts("economizer_wire_snapshot: ALL PASS");
   return 0;
}
