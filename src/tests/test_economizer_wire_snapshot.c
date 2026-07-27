#include "economizer_wire_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_pristine_copy_is_immutable(void)
{
   char source[] = "{\"model\":\"gpt-5.6\"}";
   size_t len = strlen(source);
   econ_wire_snapshot_t *snapshot = NULL;
   assert(econ_wire_snapshot_create(ECON_WIRE_OPENAI_RESPONSES, source, len, &snapshot) == 0);
   memset(source, 'x', len);
   econ_wire_bytes_t bytes = econ_wire_snapshot_bytes(snapshot);
   assert(bytes.len == len);
   assert(memcmp(bytes.data, "{\"model\":\"gpt-5.6\"}", len) == 0);
   assert(econ_wire_snapshot_route(snapshot) == ECON_WIRE_OPENAI_RESPONSES);
   econ_wire_snapshot_destroy(snapshot);
}

static void test_explicit_length_preserves_embedded_nul(void)
{
   const unsigned char source[] = {'a', 0, 'b'};
   econ_wire_snapshot_t *snapshot = NULL;
   assert(econ_wire_snapshot_create(ECON_WIRE_ANTHROPIC_MESSAGES, source, sizeof(source),
                                    &snapshot) == 0);
   econ_wire_bytes_t bytes = econ_wire_snapshot_bytes(snapshot);
   assert(bytes.len == sizeof(source));
   assert(memcmp(bytes.data, source, sizeof(source)) == 0);
   econ_wire_snapshot_destroy(snapshot);
}

static void test_invalid_inputs_fail_without_snapshot(void)
{
   econ_wire_snapshot_t *snapshot = (econ_wire_snapshot_t *)1;
   assert(econ_wire_snapshot_create(0, "{}", 2, &snapshot) == -1);
   assert(snapshot == NULL);
   snapshot = (econ_wire_snapshot_t *)1;
   assert(econ_wire_snapshot_create(ECON_WIRE_OPENAI_CHAT, NULL, 1, &snapshot) == -1);
   assert(snapshot == NULL);
   assert(econ_wire_snapshot_create(ECON_WIRE_OPENAI_CHAT, "{}", 2, NULL) == -1);
   econ_wire_snapshot_destroy(NULL);
}

static void test_off_bypasses_snapshot(void)
{
   const char body[] = "{}";
   econ_wire_snapshot_t *snapshot = (econ_wire_snapshot_t *)1;
   econ_wire_bytes_t selected = {0};
   assert(econ_wire_select(0, ECON_WIRE_OPENAI_CHAT, body, 2, &snapshot, &selected) == 0);
   assert(snapshot == NULL);
   assert(selected.data == (const uint8_t *)body);
   assert(selected.len == 2);
}

static void test_proof_gated_empty_registry_is_byte_identical_on_every_route(void)
{
   const unsigned char body[] = {'{', ' ', '"', 'x', '"', ':', ' ', '1', ' ', '}'};
   const econ_wire_route_t routes[] = {ECON_WIRE_OPENAI_CHAT, ECON_WIRE_OPENAI_RESPONSES,
                                       ECON_WIRE_ANTHROPIC_MESSAGES};
   for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
   {
      econ_wire_snapshot_t *snapshot = NULL;
      econ_wire_bytes_t selected = {0};
      assert(econ_wire_select(1, routes[i], body, sizeof(body), &snapshot, &selected) == 0);
      assert(snapshot != NULL);
      assert(selected.len == sizeof(body));
      assert(memcmp(selected.data, body, sizeof(body)) == 0);
      assert(selected.data != body);
      assert(econ_wire_snapshot_route(snapshot) == routes[i]);
      econ_wire_snapshot_destroy(snapshot);
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
