/* C-side wire coverage for the Go-owned economizer auxiliary stages. */
#include "economizer_module_client.h"

#include <aimee/audit/obs_bus.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUX_VERSION   1u
#define HEADER_LEN    12u
#define COMPACT_MAGIC 0x504d434au
#define RECALL_MAGIC  0x4c435254u
#define STATS_MAGIC   0x41545354u

static int available = 1;
static int malformed;

static uint32_t get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; i++)
      value |= (uint32_t)p[i] << (8u * i);
   return value;
}

static void put_u16(uint8_t *p, uint16_t value)
{
   p[0] = (uint8_t)value;
   p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; i++)
      p[i] = (uint8_t)(value >> (8u * i));
}

static void put_u64(uint8_t *p, uint64_t value)
{
   for (unsigned i = 0; i < 8; i++)
      p[i] = (uint8_t)(value >> (8u * i));
}

static void aux_reply(void *body, uint32_t cap, uint32_t *len, uint32_t magic, uint16_t result,
                      const void *payload, uint32_t payload_len)
{
   assert(cap >= HEADER_LEN + payload_len);
   uint8_t *out = body;
   put_u32(out, malformed ? 0u : magic);
   put_u16(out + 4, AUX_VERSION);
   put_u16(out + 6, result);
   put_u32(out + 8, payload_len);
   if (payload_len)
      memcpy(out + HEADER_LEN, payload, payload_len);
   *len = HEADER_LEN + payload_len;
}

int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return available;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id, (void)cancelled, (void)cancel_context;
   assert(deadline_ns != 0);
   if (event_kind == AIMEE_ECONOMIZER_EVENT_JSON_COMPACT)
   {
      assert(stage_id == AIMEE_ECONOMIZER_STAGE_JSON_COMPACT);
      assert(request_len == 5 && memcmp(request_body, " { } ", 5) == 0);
      aux_reply(response_body, response_capacity, response_len, COMPACT_MAGIC, 0, "{}", 2);
   }
   else if (event_kind == AIMEE_ECONOMIZER_EVENT_TOOL_RECALL)
   {
      assert(stage_id == AIMEE_ECONOMIZER_STAGE_TOOL_RECALL && request_len >= 16);
      const uint8_t *request = request_body;
      assert(get_u32(request) == RECALL_MAGIC);
      uint32_t dir_len = get_u32(request + 8), ref_len = get_u32(request + 12);
      assert(16u + dir_len + ref_len == request_len);
      assert(ref_len == 19 && memcmp(request + 16 + dir_len, "tc-0123456789abcdef", 19) == 0);
      static const uint8_t raw[] = {'r', 'a', 'w', 0xff};
      aux_reply(response_body, response_capacity, response_len, RECALL_MAGIC, 0, raw, sizeof(raw));
   }
   else if (event_kind == AIMEE_ECONOMIZER_EVENT_RECORD_BUILD)
   {
      assert(stage_id == AIMEE_ECONOMIZER_STAGE_RECORD_BUILD);
      cJSON *request = cJSON_ParseWithLength(request_body, request_len);
      assert(request != NULL && cJSON_GetNumberValue(cJSON_GetObjectItem(request, "start")) == 0);
      cJSON_Delete(request);
      const char reply[] =
          "{\"record\":{\"files_modified\":[\"src/a.c\"],\"errors_encountered\":[],"
          "\"decisions_made\":[\"[done] yes\"]},\"closet\":\"coords\\n\","
          "\"closet_evicted\":true}";
      assert(response_capacity >= sizeof(reply) - 1);
      memcpy(response_body, reply, sizeof(reply) - 1);
      *response_len = sizeof(reply) - 1;
   }
   else
   {
      assert(event_kind == AIMEE_ECONOMIZER_EVENT_TOOL_STATS);
      assert(stage_id == AIMEE_ECONOMIZER_STAGE_TOOL_STATS && request_len == 0);
      assert(response_capacity >= 72);
      uint8_t *out = response_body;
      memset(out, 0, 72);
      put_u32(out, STATS_MAGIC);
      put_u16(out + 4, AUX_VERSION);
      for (unsigned i = 0; i < 8; i++)
         put_u64(out + 8 + i * 8, i + 1);
      *response_len = 72;
   }
   return AIMEE_MODULE_CALL_OK;
}

int main(void)
{
   uint8_t *compacted = NULL;
   size_t compacted_len = 0;
   assert(econ_module_json_compact(" { } ", 5, &compacted, &compacted_len) == 0);
   assert(compacted_len == 2 && strcmp((char *)compacted, "{}") == 0);
   free(compacted);

   char *recalled = NULL;
   char err[64];
   assert(econ_module_tool_recall("relative", "tc-0123456789abcdef", &recalled, err, sizeof(err)) !=
          0);
   assert(econ_module_tool_recall("/tmp/../etc", "tc-0123456789abcdef", &recalled, err,
                                  sizeof(err)) != 0);
   assert(econ_module_tool_recall("/tmp/spills", "tc-0123456789abcdef", &recalled, err,
                                  sizeof(err)) == 0);
   assert(recalled[0] == 'r' && recalled[3] == (char)0xff && recalled[4] == '\0');
   free(recalled);

   econ_module_tool_totals_t totals;
   assert(econ_module_tool_stats(&totals) == 0);
   assert(totals.recognized == 1 && totals.recovered_bytes == 8);
   assert(totals.saved_bytes == -1 && totals.net_saved_bytes == -9);

   cJSON *messages = cJSON_Parse("[{\"role\":\"assistant\",\"content\":\"[done] yes\"}]");
   char *closet = NULL;
   int evicted = 0;
   cJSON *record = econ_module_record_build(messages, 0, 1, &closet, &evicted);
   assert(record != NULL && strcmp(closet, "coords\n") == 0);
   assert(evicted == ECON_MODULE_CLOSET_EVICT_FAIL);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(record, "decisions_made")) == 1);
   free(closet);
   cJSON_Delete(record);
   cJSON_Delete(messages);

   malformed = 1;
   assert(econ_module_json_compact(" { } ", 5, &compacted, &compacted_len) != 0);
   malformed = 0;
   available = 0;
   assert(econ_module_json_compact(" { } ", 5, &compacted, &compacted_len) != 0);

   puts("economizer_module_client: ALL PASS");
   return 0;
}
