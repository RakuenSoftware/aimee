/* conversation.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "clarify.h"
#include "db1_client/conv_context.h"
#include "db1_windows.h"
#include "payload_rewrite_state.h"
#include "db1_client/user_memory.h"
#include "wm.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static db1_family_t family = {"db1.conversation", "conversation", AIMEE_DB1_EVENT_CONVERSATION, AIMEE_DB1_STAGE_CONVERSATION, 2000, 0};

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

int db1_payload_rewrite_state_get(const char *session_id, payload_rewrite_state_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   const char *fields[] = {session_id};
   char slot1[32];
   char slot2[32];
   char slot4[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->session_id, slot1, slot2, out->last_prefix_hash, slot4, out->last_rewrite_at, slot6, slot7, slot8, out->rewrite_reason, out->updated_at};
   const size_t caps[] = {sizeof out->session_id, sizeof slot1, sizeof slot2, sizeof out->last_prefix_hash, sizeof slot4, sizeof out->last_rewrite_at, sizeof slot6, sizeof slot7, sizeof slot8, sizeof out->rewrite_reason, sizeof out->updated_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_REWRITE_STATE_GET, fields, 1, values, caps, 11, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->payload_epoch = (int64_t)strtoll(slot1, NULL, 10);
   out->compaction_epoch = (int64_t)strtoll(slot2, NULL, 10);
   out->last_payload_tokens = (int)strtol(slot4, NULL, 10);
   out->deferred_rewrite_count = (int)strtol(slot6, NULL, 10);
   out->consecutive_deferred_count = (int)strtol(slot7, NULL, 10);
   out->bytes_saved_pending = (int)strtol(slot8, NULL, 10);
   return 0;
}

int db1_wm_set(const char *session_id, const char *key, const char *value, const char *category, int ttl_seconds)
{
   if (!session_id || !session_id[0] || !key || !key[0] || !value || !value[0])
      return -1;
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", ttl_seconds);
   const char *fields[] = {session_id, key, value, category ? category : "", arg4};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WM_SET, fields, 5, NULL, NULL, 0, NULL));
}

int db1_wm_get(const char *session_id, const char *key, wm_entry_t *out)
{
   if (!session_id || !session_id[0] || !key || !key[0] || !out)
      return -1;
   const char *fields[] = {session_id, key};
   char slot0[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->session_id, out->key, out->value, out->category, out->created_at, out->updated_at, out->expires_at};
   const size_t caps[] = {sizeof slot0, sizeof out->session_id, sizeof out->key, sizeof out->value, sizeof out->category, sizeof out->created_at, sizeof out->updated_at, sizeof out->expires_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WM_GET, fields, 2, values, caps, 8, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   return 0;
}

int db1_wm_list(const char *session_id, const char *category, wm_entry_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {session_id, category ? category : "", arg2};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 8u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 8u + 2u] = out[wire_row].key;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].key;
      wire_values[wire_row * 8u + 3u] = out[wire_row].value;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].value;
      wire_values[wire_row * 8u + 4u] = out[wire_row].category;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].category;
      wire_values[wire_row * 8u + 5u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 5u] = sizeof out[wire_row].created_at;
      wire_values[wire_row * 8u + 6u] = out[wire_row].updated_at;
      wire_caps[wire_row * 8u + 6u] = sizeof out[wire_row].updated_at;
      wire_values[wire_row * 8u + 7u] = out[wire_row].expires_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].expires_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WM_LIST, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

char *db1_wm_assemble_context(const char *session_id)
{
   if (!session_id || !session_id[0])
      return NULL;
   const char *fields[] = {session_id};
   char *value = malloc(524288u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {524288u};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WM_ASSEMBLE_CONTEXT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

int db1_payload_rewrite_record(const char *session_id, int deferred, int bytes_saved, int new_payload_tokens, const char *reason, const char *new_prefix_hash)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", deferred);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", bytes_saved);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", new_payload_tokens);
   const char *fields[] = {session_id, arg1, arg2, arg3, reason ? reason : "", new_prefix_hash ? new_prefix_hash : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_REWRITE_RECORD, fields, 6, NULL, NULL, 0, NULL));
}

int db1_conv_set_chain_id(int64_t event_id_first, int64_t event_id_last, int64_t chain_id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)event_id_first);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)event_id_last);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)chain_id);
   const char *fields[] = {arg0, arg1, arg2};
   return write_result(db1_call(&family, AIMEE_DB1_OP_CONV_SET_CHAIN_ID, fields, 3, NULL, NULL, 0, NULL));
}

int64_t db1_conv_insert_chain(const char *session_id, int64_t event_id_first, int64_t event_id_last, const char *tools, const char *stub, int raw_bytes, int stub_bytes)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)event_id_first);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)event_id_last);
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%d", raw_bytes);
   char arg6[32];
   snprintf(arg6, sizeof arg6, "%d", stub_bytes);
   const char *fields[] = {session_id, arg1, arg2, tools ? tools : "", stub ? stub : "", arg5, arg6};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONV_INSERT_CHAIN, fields, 7, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_conv_pending_events(const char *session_id, conv_tool_event_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 8u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 8u + 2u] = out[wire_row].tool_name;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 8u + 3u] = out[wire_row].tool_input;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].tool_input;
      wire_values[wire_row * 8u + 4u] = out[wire_row].tool_result;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].tool_result;
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 3u + 2u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONV_PENDING_EVENTS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].result_bytes = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].chain_id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_list_chains(const char *session_id, conv_tool_chain_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 5u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 10u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 10u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 10u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 10u + 2u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 10u + 2u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 10u + 3u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 10u + 3u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 10u + 4u] = out[wire_row].tools;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].tools;
      wire_values[wire_row * 10u + 5u] = out[wire_row].stub;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].stub;
      wire_values[wire_row * 10u + 6u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 10u + 6u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 10u + 7u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 10u + 7u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 10u + 8u] = out[wire_row].state;
      wire_caps[wire_row * 10u + 8u] = sizeof out[wire_row].state;
      wire_values[wire_row * 10u + 9u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONV_LIST_CHAINS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 10), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 10u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 10u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].event_id_first = (int64_t)strtoll(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].event_id_last = (int64_t)strtoll(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].raw_bytes = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      out[wire_row].stub_bytes = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_chain_events(int64_t chain_id, conv_tool_event_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)chain_id);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 8u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 8u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 8u + 0u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 8u + 0u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 8u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 8u + 2u] = out[wire_row].tool_name;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].tool_name;
      wire_values[wire_row * 8u + 3u] = out[wire_row].tool_input;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].tool_input;
      wire_values[wire_row * 8u + 4u] = out[wire_row].tool_result;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].tool_result;
      wire_values[wire_row * 8u + 5u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 8u + 5u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 3u + 2u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].created_at;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONV_CHAIN_EVENTS, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 8), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 8u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 8u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].result_bytes = (int)strtol(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].chain_id = (int64_t)strtoll(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_search_chains(const char *session_id, const char *query, conv_tool_chain_t *out, int max)
{
   if (!session_id || !session_id[0] || !query || !query[0] || !out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", max);
   const char *fields[] = {session_id, query, arg2};
   char **wire_values = malloc((size_t)max * 10u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 10u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 5u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 10u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 10u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 10u + 1u] = out[wire_row].session_id;
      wire_caps[wire_row * 10u + 1u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 10u + 2u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 10u + 2u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 10u + 3u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 10u + 3u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 10u + 4u] = out[wire_row].tools;
      wire_caps[wire_row * 10u + 4u] = sizeof out[wire_row].tools;
      wire_values[wire_row * 10u + 5u] = out[wire_row].stub;
      wire_caps[wire_row * 10u + 5u] = sizeof out[wire_row].stub;
      wire_values[wire_row * 10u + 6u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 10u + 6u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 10u + 7u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 10u + 7u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 10u + 8u] = out[wire_row].state;
      wire_caps[wire_row * 10u + 8u] = sizeof out[wire_row].state;
      wire_values[wire_row * 10u + 9u] = out[wire_row].created_at;
      wire_caps[wire_row * 10u + 9u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONV_SEARCH_CHAINS, fields, 3, wire_values, wire_caps,
                           (uint32_t)(max * 10), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 10u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 10u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      out[wire_row].event_id_first = (int64_t)strtoll(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      out[wire_row].event_id_last = (int64_t)strtoll(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      out[wire_row].raw_bytes = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      out[wire_row].stub_bytes = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_conv_state_get(const char *session_id, int64_t *last_event_id_out, int *chain_count_out, int *event_count_out)
{
   if (!session_id || !session_id[0] || !last_event_id_out || !chain_count_out || !event_count_out)
      return -1;
   const char *fields[] = {session_id};
   char slot0[32];
   char slot1[32];
   char slot2[32];
   char *const values[] = {slot0, slot1, slot2};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONV_STATE_GET, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *last_event_id_out = (int64_t)strtoll(slot0, NULL, 10);
   *chain_count_out = (int)strtol(slot1, NULL, 10);
   *event_count_out = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_conv_state_update(const char *session_id, int64_t last_event_id, int chain_count, int event_count)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)last_event_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%d", chain_count);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", event_count);
   const char *fields[] = {session_id, arg1, arg2, arg3};
   return write_result(db1_call(&family, AIMEE_DB1_OP_CONV_STATE_UPDATE, fields, 4, NULL, NULL, 0, NULL));
}

int db1_clarify_start(const char *description, clarify_session_t *out)
{
   if (!description || !description[0] || !out)
      return -1;
   const char *fields[] = {description};
   char slot0[32];
   char slot2[32];
   char slot3[32];
   char slot7[32];
   char slot8[32];
   char slot12[32];
   char slot13[32];
   char slot17[32];
   char slot18[32];
   char slot22[32];
   char slot23[32];
   char slot27[32];
   char slot28[32];
   char slot32[32];
   char slot33[32];
   char slot37[32];
   char slot38[32];
   char slot42[32];
   char slot43[32];
   char slot44[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->description, slot2, slot3, out->qa[0].dimension, out->qa[0].question, out->qa[0].answer, slot7, slot8, out->qa[1].dimension, out->qa[1].question, out->qa[1].answer, slot12, slot13, out->qa[2].dimension, out->qa[2].question, out->qa[2].answer, slot17, slot18, out->qa[3].dimension, out->qa[3].question, out->qa[3].answer, slot22, slot23, out->qa[4].dimension, out->qa[4].question, out->qa[4].answer, slot27, slot28, out->qa[5].dimension, out->qa[5].question, out->qa[5].answer, slot32, slot33, out->qa[6].dimension, out->qa[6].question, out->qa[6].answer, slot37, slot38, out->qa[7].dimension, out->qa[7].question, out->qa[7].answer, slot42, slot43, slot44, out->spec, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->description, sizeof slot2, sizeof slot3, sizeof out->qa[0].dimension, sizeof out->qa[0].question, sizeof out->qa[0].answer, sizeof slot7, sizeof slot8, sizeof out->qa[1].dimension, sizeof out->qa[1].question, sizeof out->qa[1].answer, sizeof slot12, sizeof slot13, sizeof out->qa[2].dimension, sizeof out->qa[2].question, sizeof out->qa[2].answer, sizeof slot17, sizeof slot18, sizeof out->qa[3].dimension, sizeof out->qa[3].question, sizeof out->qa[3].answer, sizeof slot22, sizeof slot23, sizeof out->qa[4].dimension, sizeof out->qa[4].question, sizeof out->qa[4].answer, sizeof slot27, sizeof slot28, sizeof out->qa[5].dimension, sizeof out->qa[5].question, sizeof out->qa[5].answer, sizeof slot32, sizeof slot33, sizeof out->qa[6].dimension, sizeof out->qa[6].question, sizeof out->qa[6].answer, sizeof slot37, sizeof slot38, sizeof out->qa[7].dimension, sizeof out->qa[7].question, sizeof out->qa[7].answer, sizeof slot42, sizeof slot43, sizeof slot44, sizeof out->spec, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CLARIFY_START, fields, 1, values, caps, 48, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->status = (int)strtol(slot2, NULL, 10);
   out->score = (float)strtod(slot3, NULL);
   out->qa[0].answered = (int)strtol(slot7, NULL, 10);
   out->qa[0].seq = (int)strtol(slot8, NULL, 10);
   out->qa[1].answered = (int)strtol(slot12, NULL, 10);
   out->qa[1].seq = (int)strtol(slot13, NULL, 10);
   out->qa[2].answered = (int)strtol(slot17, NULL, 10);
   out->qa[2].seq = (int)strtol(slot18, NULL, 10);
   out->qa[3].answered = (int)strtol(slot22, NULL, 10);
   out->qa[3].seq = (int)strtol(slot23, NULL, 10);
   out->qa[4].answered = (int)strtol(slot27, NULL, 10);
   out->qa[4].seq = (int)strtol(slot28, NULL, 10);
   out->qa[5].answered = (int)strtol(slot32, NULL, 10);
   out->qa[5].seq = (int)strtol(slot33, NULL, 10);
   out->qa[6].answered = (int)strtol(slot37, NULL, 10);
   out->qa[6].seq = (int)strtol(slot38, NULL, 10);
   out->qa[7].answered = (int)strtol(slot42, NULL, 10);
   out->qa[7].seq = (int)strtol(slot43, NULL, 10);
   out->qa_count = (int)strtol(slot44, NULL, 10);
   return out->id;
}

int db1_clarify_get(int id, clarify_session_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot2[32];
   char slot3[32];
   char slot7[32];
   char slot8[32];
   char slot12[32];
   char slot13[32];
   char slot17[32];
   char slot18[32];
   char slot22[32];
   char slot23[32];
   char slot27[32];
   char slot28[32];
   char slot32[32];
   char slot33[32];
   char slot37[32];
   char slot38[32];
   char slot42[32];
   char slot43[32];
   char slot44[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->description, slot2, slot3, out->qa[0].dimension, out->qa[0].question, out->qa[0].answer, slot7, slot8, out->qa[1].dimension, out->qa[1].question, out->qa[1].answer, slot12, slot13, out->qa[2].dimension, out->qa[2].question, out->qa[2].answer, slot17, slot18, out->qa[3].dimension, out->qa[3].question, out->qa[3].answer, slot22, slot23, out->qa[4].dimension, out->qa[4].question, out->qa[4].answer, slot27, slot28, out->qa[5].dimension, out->qa[5].question, out->qa[5].answer, slot32, slot33, out->qa[6].dimension, out->qa[6].question, out->qa[6].answer, slot37, slot38, out->qa[7].dimension, out->qa[7].question, out->qa[7].answer, slot42, slot43, slot44, out->spec, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->description, sizeof slot2, sizeof slot3, sizeof out->qa[0].dimension, sizeof out->qa[0].question, sizeof out->qa[0].answer, sizeof slot7, sizeof slot8, sizeof out->qa[1].dimension, sizeof out->qa[1].question, sizeof out->qa[1].answer, sizeof slot12, sizeof slot13, sizeof out->qa[2].dimension, sizeof out->qa[2].question, sizeof out->qa[2].answer, sizeof slot17, sizeof slot18, sizeof out->qa[3].dimension, sizeof out->qa[3].question, sizeof out->qa[3].answer, sizeof slot22, sizeof slot23, sizeof out->qa[4].dimension, sizeof out->qa[4].question, sizeof out->qa[4].answer, sizeof slot27, sizeof slot28, sizeof out->qa[5].dimension, sizeof out->qa[5].question, sizeof out->qa[5].answer, sizeof slot32, sizeof slot33, sizeof out->qa[6].dimension, sizeof out->qa[6].question, sizeof out->qa[6].answer, sizeof slot37, sizeof slot38, sizeof out->qa[7].dimension, sizeof out->qa[7].question, sizeof out->qa[7].answer, sizeof slot42, sizeof slot43, sizeof slot44, sizeof out->spec, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CLARIFY_GET, fields, 1, values, caps, 48, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->status = (int)strtol(slot2, NULL, 10);
   out->score = (float)strtod(slot3, NULL);
   out->qa[0].answered = (int)strtol(slot7, NULL, 10);
   out->qa[0].seq = (int)strtol(slot8, NULL, 10);
   out->qa[1].answered = (int)strtol(slot12, NULL, 10);
   out->qa[1].seq = (int)strtol(slot13, NULL, 10);
   out->qa[2].answered = (int)strtol(slot17, NULL, 10);
   out->qa[2].seq = (int)strtol(slot18, NULL, 10);
   out->qa[3].answered = (int)strtol(slot22, NULL, 10);
   out->qa[3].seq = (int)strtol(slot23, NULL, 10);
   out->qa[4].answered = (int)strtol(slot27, NULL, 10);
   out->qa[4].seq = (int)strtol(slot28, NULL, 10);
   out->qa[5].answered = (int)strtol(slot32, NULL, 10);
   out->qa[5].seq = (int)strtol(slot33, NULL, 10);
   out->qa[6].answered = (int)strtol(slot37, NULL, 10);
   out->qa[6].seq = (int)strtol(slot38, NULL, 10);
   out->qa[7].answered = (int)strtol(slot42, NULL, 10);
   out->qa[7].seq = (int)strtol(slot43, NULL, 10);
   out->qa_count = (int)strtol(slot44, NULL, 10);
   return 0;
}

int db1_clarify_answer(int id, const char *answer, clarify_session_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, answer ? answer : ""};
   char slot0[32];
   char slot2[32];
   char slot3[32];
   char slot7[32];
   char slot8[32];
   char slot12[32];
   char slot13[32];
   char slot17[32];
   char slot18[32];
   char slot22[32];
   char slot23[32];
   char slot27[32];
   char slot28[32];
   char slot32[32];
   char slot33[32];
   char slot37[32];
   char slot38[32];
   char slot42[32];
   char slot43[32];
   char slot44[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->description, slot2, slot3, out->qa[0].dimension, out->qa[0].question, out->qa[0].answer, slot7, slot8, out->qa[1].dimension, out->qa[1].question, out->qa[1].answer, slot12, slot13, out->qa[2].dimension, out->qa[2].question, out->qa[2].answer, slot17, slot18, out->qa[3].dimension, out->qa[3].question, out->qa[3].answer, slot22, slot23, out->qa[4].dimension, out->qa[4].question, out->qa[4].answer, slot27, slot28, out->qa[5].dimension, out->qa[5].question, out->qa[5].answer, slot32, slot33, out->qa[6].dimension, out->qa[6].question, out->qa[6].answer, slot37, slot38, out->qa[7].dimension, out->qa[7].question, out->qa[7].answer, slot42, slot43, slot44, out->spec, out->created_at, out->updated_at};
   const size_t caps[] = {sizeof slot0, sizeof out->description, sizeof slot2, sizeof slot3, sizeof out->qa[0].dimension, sizeof out->qa[0].question, sizeof out->qa[0].answer, sizeof slot7, sizeof slot8, sizeof out->qa[1].dimension, sizeof out->qa[1].question, sizeof out->qa[1].answer, sizeof slot12, sizeof slot13, sizeof out->qa[2].dimension, sizeof out->qa[2].question, sizeof out->qa[2].answer, sizeof slot17, sizeof slot18, sizeof out->qa[3].dimension, sizeof out->qa[3].question, sizeof out->qa[3].answer, sizeof slot22, sizeof slot23, sizeof out->qa[4].dimension, sizeof out->qa[4].question, sizeof out->qa[4].answer, sizeof slot27, sizeof slot28, sizeof out->qa[5].dimension, sizeof out->qa[5].question, sizeof out->qa[5].answer, sizeof slot32, sizeof slot33, sizeof out->qa[6].dimension, sizeof out->qa[6].question, sizeof out->qa[6].answer, sizeof slot37, sizeof slot38, sizeof out->qa[7].dimension, sizeof out->qa[7].question, sizeof out->qa[7].answer, sizeof slot42, sizeof slot43, sizeof slot44, sizeof out->spec, sizeof out->created_at, sizeof out->updated_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CLARIFY_ANSWER, fields, 2, values, caps, 48, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int)strtol(slot0, NULL, 10);
   out->status = (int)strtol(slot2, NULL, 10);
   out->score = (float)strtod(slot3, NULL);
   out->qa[0].answered = (int)strtol(slot7, NULL, 10);
   out->qa[0].seq = (int)strtol(slot8, NULL, 10);
   out->qa[1].answered = (int)strtol(slot12, NULL, 10);
   out->qa[1].seq = (int)strtol(slot13, NULL, 10);
   out->qa[2].answered = (int)strtol(slot17, NULL, 10);
   out->qa[2].seq = (int)strtol(slot18, NULL, 10);
   out->qa[3].answered = (int)strtol(slot22, NULL, 10);
   out->qa[3].seq = (int)strtol(slot23, NULL, 10);
   out->qa[4].answered = (int)strtol(slot27, NULL, 10);
   out->qa[4].seq = (int)strtol(slot28, NULL, 10);
   out->qa[5].answered = (int)strtol(slot32, NULL, 10);
   out->qa[5].seq = (int)strtol(slot33, NULL, 10);
   out->qa[6].answered = (int)strtol(slot37, NULL, 10);
   out->qa[6].seq = (int)strtol(slot38, NULL, 10);
   out->qa[7].answered = (int)strtol(slot42, NULL, 10);
   out->qa[7].seq = (int)strtol(slot43, NULL, 10);
   out->qa_count = (int)strtol(slot44, NULL, 10);
   return 0;
}

/* clang-format on */
