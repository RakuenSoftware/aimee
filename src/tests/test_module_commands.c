/* Fixed-module discovery uses the same invocation transport as plugins, with
 * the invocation stage declared by the owner. Exercise the production native
 * decoder/dispatcher against a deterministic bus peer. */
#include "module_commands.h"
#include "command_registry.h"
#include "aimee/audit/obs_bus.h"
#include "log.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fixed_present = 1, malformed, empty, reset_during_call;
static uint32_t last_kind, last_stage;
static int expect_context;
static const char *expect_verb = "stats";
static void put32(unsigned char *out, uint32_t v)
{
   out[0] = v;
   out[1] = v >> 8;
   out[2] = v >> 16;
   out[3] = v >> 24;
}
static uint32_t get32(const unsigned char *p)
{
   return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int obs_bus_module_available(uint32_t kind)
{
   return (fixed_present && kind == 6143) || kind == AIMEE_PLUGIN_KIND(201, 2);
}
aimee_module_call_result_t
obs_bus_module_call(uint32_t kind, uint32_t stage, uint64_t trace, uint64_t deadline,
                    const void *body, uint32_t body_len, void *result, uint32_t capacity,
                    uint32_t *result_len, aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace;
   (void)deadline;
   (void)cancelled;
   (void)cancel_context;
   const unsigned char *req = body;
   unsigned char *out = result;
   assert(capacity >= 128);
   memset(out, 0, 128);
   if (stage == 255 || stage == 2)
   {
      assert(body_len == 8 && get32(req) == 0x444d4344u);
      unsigned version = stage == 255 ? 2 : 1;
      assert(get32(req + 4) == version);
      assert(kind == (version == 2 ? 6143 : AIMEE_PLUGIN_KIND(201, 2)));
      put32(out, 0x524d4344u);
      put32(out + 4, version);
      put32(out + 8, version == 2 ? (empty ? 0 : 2) : 1);
      unsigned offset = version == 2 ? 16 : 12;
      if (version == 2)
         put32(out + 12, malformed == 2 ? 255 : 8);
      if (version == 2 && empty)
      {
         *result_len = offset;
         return AIMEE_MODULE_CALL_OK;
      }
      put32(out + offset, AIMEE_SURFACE_RPC);
      put32(out + offset + 4, AIMEE_MCP_DISCOVERABLE);
      out[offset + 8] = 6;
      out[offset + 10] = 5;
      out[offset + 12] = 10;
      if (version == 2 && malformed == 3)
         out[offset + 14] = 1;
      memcpy(out + offset + 16, version == 2 ? "memory" : "plugin", 6);
      memcpy(out + offset + 22, "statsStatistics", 15);
      *result_len = offset + 37;
      if (version == 2)
      {
         memcpy(out + offset + 37, out + offset, 37);
         put32(out + offset + 37, 0); /* host-only */
         out[offset + 47] = 10;
         memcpy(out + offset + 59, "embed_textStatistics", 20);
         *result_len += 42;
      }
      if (version == 2 && malformed == 4)
      {
         put32(out + 8, 2);
         memcpy(out + offset + 37, out + offset, 37);
         *result_len = offset + 74;
      }
      if (version == 2 && malformed == 1)
         --*result_len;
      return AIMEE_MODULE_CALL_OK;
   }
   assert(get32(req) == 0x51504d43u && get32(req + 4) == (expect_context ? 2u : 1u));
   unsigned header = expect_context ? 20 : 16;
   assert(body_len >= header + 5 && memcmp(req + header, expect_verb, 5) == 0);
   if (expect_context)
   {
      const char expected[] = "{\"authenticated\":true,\"principal\":\"user:alice\"}";
      assert(get32(req + 16) == sizeof(expected) - 1);
      assert(body_len == header + 5 + get32(req + 12) + sizeof(expected) - 1);
      assert(memcmp(req + header + 5 + get32(req + 12), expected, sizeof(expected) - 1) == 0);
   }
   last_kind = kind;
   last_stage = stage;
   if (reset_during_call)
      aimee_module_commands_reset();
   const char json[] = "{\"status\":\"ok\"}";
   put32(out, 0x53504d43u);
   put32(out + 4, 1);
   put32(out + 8, sizeof json - 1);
   memcpy(out + 12, json, sizeof json - 1);
   *result_len = 12 + sizeof json - 1;
   return AIMEE_MODULE_CALL_OK;
}
uint64_t aimee_module_call_deadline_ns(int timeout_ms)
{
   return (uint64_t)timeout_ms;
}
const char *aimee_module_call_result_name(aimee_module_call_result_t result)
{
   (void)result;
   return "test";
}
void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}
int mcp_osv_gate_blocks_argv(const char *name, int argc, const char *const argv[])
{
   (void)name;
   (void)argc;
   (void)argv;
   return 0;
}

int main(void)
{
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   assert(aimee_module_commands_dispatch("memory.stats", args, &reply) == 1);
   assert(last_kind == 5896 && last_stage == 8);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status")), "ok") ==
          0);
   cJSON_Delete(reply);
   cJSON *context = cJSON_Parse("{\"authenticated\":true,\"principal\":\"user:alice\"}");
   expect_context = 1;
   assert(aimee_module_commands_dispatch_context("memory.stats", args, context, &reply) == 1);
   cJSON_Delete(reply);
   expect_context = 0;
   assert(aimee_module_commands_dispatch_context("plugin.stats", args, context, &reply) == 1);
   cJSON_Delete(reply);
   cJSON_Delete(context);
   assert(aimee_module_commands_dispatch("plugin.stats", args, &reply) == 1);
   assert(last_kind == AIMEE_PLUGIN_KIND(201, 1) && last_stage == 1);
   cJSON_Delete(reply);
   assert(aimee_module_commands_plugin_count() == 1);
   assert(aimee_command_count() == 2);
   assert(aimee_command_find_method("memory.embed_text") == NULL);
   assert(aimee_module_commands_dispatch("memory.embed_text", args, &reply) == 0 && reply == NULL);
   assert(aimee_module_commands_dispatch_internal("memory.stats", args, &reply) == 0);
   expect_verb = "embed_text";
   assert(aimee_module_commands_dispatch_internal("memory.embed_text", args, &reply) == 1);
   assert(last_kind == 5896 && last_stage == 8);
   cJSON_Delete(reply);
   for (malformed = 1; malformed <= 4; ++malformed)
   {
      assert(aimee_module_commands_collect() == 1);
      assert(aimee_command_find_method("memory.stats") != NULL);
      assert(aimee_module_commands_dispatch_internal("memory.embed_text", args, &reply) == 1);
      cJSON_Delete(reply);
   }
   malformed = 0;
   empty = 1;
   assert(aimee_module_commands_collect() == 1);
   assert(aimee_command_find_method("memory.stats") == NULL);
   assert(aimee_module_commands_dispatch_internal("memory.embed_text", args, &reply) == 0);
   empty = 0;
   assert(aimee_module_commands_collect() == 2);
   fixed_present = 0;
   assert(aimee_module_commands_collect() == 1);
   assert(aimee_command_find_method("memory.stats") == NULL);
   assert(aimee_module_commands_dispatch_internal("memory.embed_text", args, &reply) == 0);
   fixed_present = 1;
   assert(aimee_module_commands_collect() == 2);
   reset_during_call = 1;
   assert(aimee_module_commands_dispatch_internal("memory.embed_text", args, &reply) == 1);
   cJSON_Delete(reply);
   expect_verb = "stats";
   assert(aimee_module_commands_dispatch("memory.stats", args, &reply) == 1);
   cJSON_Delete(reply);
   cJSON_Delete(args);
   assert(aimee_command_count() == 0);
   puts("module commands: ok");
   return 0;
}
