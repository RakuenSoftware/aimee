#include "economizer_wire_snapshot.h"

#include "economizer_proof.h"

#include <stdlib.h>
#include <string.h>

struct econ_wire_snapshot
{
   econ_wire_route_t route;
   uint8_t *data;
   size_t len;
};

static int route_valid(econ_wire_route_t route)
{
   return route == ECON_WIRE_OPENAI_CHAT || route == ECON_WIRE_OPENAI_RESPONSES ||
          route == ECON_WIRE_ANTHROPIC_MESSAGES;
}

int econ_wire_snapshot_create(econ_wire_route_t route, const void *pristine, size_t pristine_len,
                              econ_wire_snapshot_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!route_valid(route) || (!pristine && pristine_len != 0) ||
       !econ_registry_signature_valid() || econ_registry_entry_count() != 0)
      return -1;
   if (pristine_len == SIZE_MAX)
      return -1;

   econ_wire_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
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

int econ_wire_select(int proof_gated, econ_wire_route_t route, const void *pristine,
                     size_t pristine_len, econ_wire_snapshot_t **snapshot,
                     econ_wire_bytes_t *selected)
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
   if (econ_wire_snapshot_create(route, pristine, pristine_len, snapshot) != 0)
      return -1;
   *selected = econ_wire_snapshot_bytes(*snapshot);
   return 0;
}

econ_wire_route_t econ_wire_snapshot_route(const econ_wire_snapshot_t *snapshot)
{
   return snapshot ? snapshot->route : 0;
}

econ_wire_bytes_t econ_wire_snapshot_bytes(const econ_wire_snapshot_t *snapshot)
{
   econ_wire_bytes_t bytes = {0};
   if (snapshot)
   {
      bytes.data = snapshot->data;
      bytes.len = snapshot->len;
   }
   return bytes;
}

void econ_wire_snapshot_destroy(econ_wire_snapshot_t *snapshot)
{
   if (!snapshot)
      return;
   free(snapshot->data);
   snapshot->data = NULL;
   snapshot->len = 0;
   free(snapshot);
}
