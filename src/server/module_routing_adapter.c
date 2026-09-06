#define _POSIX_C_SOURCE 200809L

#include "module_routing_adapter.h"

#include "agent_config.h"
#include "log.h"
#include "model_registry.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/audit/obs_bus.h>
#include <aimee/routing/module_api.h>

#include <stdatomic.h>
#include <time.h>

#define ROUTING_DEADLINE_NS (500ULL * 1000000ULL)

static atomic_uint_fast64_t next_trace = 1;

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int select_via_module(int randomized, uint32_t candidate_count, uint32_t *selected_index)
{
   uint8_t request[AIMEE_ROUTING_REQUEST_LEN];
   uint8_t response[AIMEE_ROUTING_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_routing_select_mode_t mode =
       randomized ? AIMEE_ROUTING_SELECT_RANDOMIZED : AIMEE_ROUTING_SELECT_BALANCED;
   if (!selected_index ||
       aimee_routing_request_encode(mode, candidate_count, request, sizeof(request)) != 0)
      return -1;
   uint64_t now = monotonic_ns();
   if (!now)
      return -1;
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   if (trace == 0)
      trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   aimee_module_call_result_t result = obs_bus_module_call(
       AIMEE_ROUTING_EVENT_KIND, AIMEE_ROUTING_STAGE_SELECT, trace, now + ROUTING_DEADLINE_NS,
       request, sizeof(request), response, sizeof(response), &response_len, NULL, NULL);
   if (result != AIMEE_MODULE_CALL_OK ||
       aimee_routing_response_decode(response, response_len, candidate_count, selected_index) != 0)
   {
      /* ERROR, not WARN: the caller turns this into a NULL agent, which reads
       * downstream as "no eligible agent" -- the wrong diagnosis entirely. The
       * module being absent or slow is a deployment fault and has to say so in
       * its own words, or an operator hunts the agent roster while the routing
       * module is what is down. */
      aimee_log(LOG_ERROR, "routing",
                "routing module call failed (%s): refusing to select among %u candidates; this is "
                "a routing-module fault, not an empty agent roster",
                aimee_module_call_result_name(result), candidate_count);
      return -1;
   }
   return 0;
}

static void add_rate(cJSON *obj, const char *key, double rate, int known)
{
   if (known && isfinite(rate) && rate >= 0)
      cJSON_AddNumberToObject(obj, key, rate);
}

/* The server supplies facts; the Go routing process owns their cost ordering.
 * Never include credentials, endpoints, or prompt text in this request. */
static int cost_via_module(const agent_config_t *cfg, const char *role, agent_t *const candidates[],
                           int count, int min_context)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddNumberToObject(request, "version", 1);
   int input = cfg->route_input_tokens > 0 ? cfg->route_input_tokens : 4096;
   if (min_context > input)
      input = min_context;
   cJSON_AddNumberToObject(request, "input_tokens", input);
   cJSON_AddNumberToObject(request, "output_tokens",
                           cfg->route_output_tokens > 0 ? cfg->route_output_tokens : 4096);
   cJSON_AddBoolToObject(request, "premium", cfg->route_premium);
   cJSON *items = cJSON_AddArrayToObject(request, "candidates");
   for (int i = 0; i < count; i++)
   {
      const agent_t *ag = candidates[i];
      model_capability_t cap = {0};
      int have = model_capability_get(agent_catalog_provider(ag), ag->model, &cap);
      cJSON *item = cJSON_CreateObject();
      cJSON_AddItemToArray(items, item);
      cJSON_AddStringToObject(item, "name", ag->name);
      cJSON_AddNumberToObject(item, "tier", ag->cost_tier);
      cJSON_AddNumberToObject(item, "competence", agent_role_competence(ag, role));
      cJSON *prices = cJSON_AddObjectToObject(item, "prices");
      /* Catalog zero has no presence bit: only a declaration can assert free. */
      add_rate(prices, "input", cap.cost_in_per_mtok, have && cap.cost_in_per_mtok > 0);
      add_rate(prices, "output", cap.cost_out_per_mtok, have && cap.cost_out_per_mtok > 0);
      cJSON *overrides = cJSON_AddObjectToObject(item, "overrides");
      add_rate(overrides, "input", ag->price_in_per_mtok, ag->declared & AGENT_DECL_PRICE_IN);
      add_rate(overrides, "output", ag->price_out_per_mtok, ag->declared & AGENT_DECL_PRICE_OUT);
      cJSON_AddBoolToObject(item, "truncated", cap.price_bands_truncated);
      cJSON *bands = cJSON_AddArrayToObject(item, "bands");
      for (int j = 0; have && j < cap.price_band_count && j < MODEL_PRICE_BANDS_MAX; j++)
      {
         const model_price_band_t *b = &cap.price_bands[j];
         cJSON *band = cJSON_CreateObject();
         cJSON_AddItemToArray(bands, band);
         cJSON_AddNumberToObject(band, "above", b->above_tokens);
         add_rate(band, "input", b->in_per_mtok, b->in_per_mtok > 0);
         add_rate(band, "output", b->out_per_mtok, b->out_per_mtok > 0);
      }
   }
   char *body = cJSON_PrintUnformatted(request);
   cJSON_Delete(request);
   if (!body)
      return -1;
   uint8_t response[128];
   uint32_t response_len = 0;
   uint64_t now = monotonic_ns();
   uint64_t trace = atomic_fetch_add_explicit(&next_trace, 1, memory_order_relaxed);
   aimee_module_call_result_t result =
       now ? obs_bus_module_call(AIMEE_ROUTING_EVENT_PLAN, AIMEE_ROUTING_STAGE_PLAN, trace,
                                 now + ROUTING_DEADLINE_NS, (const uint8_t *)body,
                                 (uint32_t)strlen(body), response, sizeof(response) - 1,
                                 &response_len, NULL, NULL)
           : AIMEE_MODULE_CALL_INTERNAL;
   free(body);
   if (result != AIMEE_MODULE_CALL_OK || response_len >= sizeof(response))
   {
      aimee_log(LOG_ERROR, "routing", "cost selection module failed; refusing an unranked route");
      return -1;
   }
   response[response_len] = 0;
   cJSON *reply = cJSON_Parse((const char *)response);
   cJSON *selected = cJSON_GetObjectItemCaseSensitive(reply, "selected");
   int index = cJSON_IsNumber(selected) && selected->valuedouble == selected->valueint
                   ? selected->valueint
                   : -1;
   cJSON_Delete(reply);
   if (index < 0 || index >= count)
      return -1;
   aimee_log(LOG_INFO, "routing", "selected '%s' for role '%s' (strategy=%s, estimated_input=%d)",
             candidates[index]->name, role ? role : "", cfg->route_premium ? "competence" : "cost",
             input);
   return index;
}

void server_module_routing_configure(void)
{
   agent_set_route_selection_provider(select_via_module);
   agent_set_route_cost_provider(cost_via_module);
}
