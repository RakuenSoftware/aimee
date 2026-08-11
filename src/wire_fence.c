#include "wire_fence.h"

#include <stdlib.h>
#include <string.h>

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

int wire_fence_select(int proof_gated, wire_fence_route_t route, const void *pristine,
                      size_t pristine_len, wire_fence_t **snapshot, wire_fence_bytes_t *selected)
{
   if (!snapshot || !selected || (!pristine && pristine_len != 0) || !route_valid(route))
      return -1;
   *snapshot = NULL;
   selected->data = NULL;
   selected->len = 0;
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
