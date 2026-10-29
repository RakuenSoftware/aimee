/* runtime.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "caches.h"
#include "decisions.h"
#include "env.h"
#include "fsnap.h"
#include "local_operator.h"
#include "mcp_osv_cache.h"
#include "model_catalog.h"
#include "db1_client/model_pricing.h"
#include "project_clones.h"
#include "runtime_state.h"
#include "tool_local_availability.h"
#include "web_page_cache.h"
#include "working_profile_local.h"

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

static db1_family_t family = {"db1.runtime", "runtime", AIMEE_DB1_EVENT_RUNTIME, AIMEE_DB1_STAGE_RUNTIME, 2000, 0};

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

int db1_runtime_state_set(const char *key, const char *value)
{
   if (!key || !key[0])
      return -1;
   const char *fields[] = {key, value ? value : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_RUNTIME_STATE_SET, fields, 2, NULL, NULL, 0, NULL));
}

int db1_runtime_state_get(const char *key, char *out, size_t out_len)
{
   if (!key || !key[0] || !out || out_len == 0)
      return -1;
   const char *fields[] = {key};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_len, sizeof slot_rc};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_RUNTIME_STATE_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_runtime_state_lookup(const char *key, char *out, size_t out_len)
{
   if (!out || !out_len)
      return -1;
   out[0] = 0;
   if (!key || !key[0])
      return -1;
   const char *fields[] = {key};
   char slot_rc[32] = "";
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_len, sizeof slot_rc};
   int status = db1_call(&family, AIMEE_DB1_OP_RUNTIME_STATE_GET, fields, 1, values, caps, 2, NULL);
   if (status == (int)AIMEE_DB1_STATUS_OK && !strcmp(slot_rc, "0"))
      return 0;
   out[0] = 0;
   return status == (int)AIMEE_DB1_STATUS_MISSING ? 1 : -1;
}

int db1_local_operator_get_active(db1_local_operator_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->secret_ref, out->operator_uuid, slot2, out->display_hint, out->created_at};
   const size_t caps[] = {sizeof out->secret_ref, sizeof out->operator_uuid, sizeof slot2, sizeof out->display_hint, sizeof out->created_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_LOCAL_OPERATOR_GET_ACTIVE, fields, 0, values, caps, 5, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->active = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_env_capability_list(db1_env_capability_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 3u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 3u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 3u + 0u] = out[wire_row].key;
      wire_caps[wire_row * 3u + 0u] = sizeof out[wire_row].key;
      wire_values[wire_row * 3u + 1u] = out[wire_row].value;
      wire_caps[wire_row * 3u + 1u] = sizeof out[wire_row].value;
      wire_values[wire_row * 3u + 2u] = out[wire_row].detected_at;
      wire_caps[wire_row * 3u + 2u] = sizeof out[wire_row].detected_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_ENV_CAPABILITY_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 3), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 3u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 3u);
   return wire_rows;
}

int db1_model_price_get(const char *model, double *in_per_mtok, double *out_per_mtok)
{
   if (!model || !model[0] || !in_per_mtok || !out_per_mtok)
      return -1;
   const char *fields[] = {model};
   char slot0[32];
   char slot1[32];
   char *const values[] = {slot0, slot1};
   const size_t caps[] = {sizeof slot0, sizeof slot1};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MODEL_PRICE_GET, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *in_per_mtok = strtod(slot0, NULL);
   *out_per_mtok = strtod(slot1, NULL);
   return 0;
}

int db1_working_profile_local_observe(const char *field, const char *value, double confidence, const char *session_id, int threshold)
{
   if (!field || !field[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)confidence);
   char arg4[32];
   snprintf(arg4, sizeof arg4, "%d", threshold);
   const char *fields[] = {field, value ? value : "", arg2, session_id ? session_id : "", arg4};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_OBSERVE, fields, 5, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_working_profile_local_list(db1_working_profile_local_state_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 128)
      max = 128;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 2u * sizeof *wire_scratch);
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
      wire_values[wire_row * 5u + 0u] = out[wire_row].field;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].field;
      wire_values[wire_row * 5u + 1u] = out[wire_row].value;
      wire_caps[wire_row * 5u + 1u] = sizeof out[wire_row].value;
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 5u + 4u] = out[wire_row].updated_at;
      wire_caps[wire_row * 5u + 4u] = sizeof out[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 5u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 5u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].observation_count = (int)strtol(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].score = strtod(wire_scratch[wire_row * 2u + 1u], NULL);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_context_snapshot_insert_turn(const char *session_id, int64_t memory_id,
                                     double relevance_score, int64_t turn_index)
{
   if (!session_id || !session_id[0] || memory_id <= 0 || turn_index <= 0)
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)memory_id);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%.17g", (double)relevance_score);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%lld", (long long)turn_index);
   const char *fields[] = {session_id, arg1, arg2, arg3};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONTEXT_SNAPSHOT_INSERT_TURN, fields, 4, NULL, NULL, 0, NULL);
   return wire_status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

int db1_context_snapshot_activation(const char *session_id,
                                    char (*out)[DB1_CONTEXT_ACTIVATION_ROW_LEN], int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (max > 512)
      max = 512;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {session_id, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CONTEXT_SNAPSHOT_ACTIVATION, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   return (int)(wire_filled / 1u);
}

char *db1_agent_cache_get(const char *role, const char *prompt)
{
   const char *fields[] = {role ? role : "", prompt ? prompt : ""};
   char *value = malloc(1048576u);
   if (!value)
      return NULL;
   char *const values[] = {value};
   const size_t caps[] = {1048576u};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_AGENT_CACHE_GET, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

void db1_agent_cache_put(const char *role, const char *prompt, const char *result)
{
   const char *fields[] = {role ? role : "", prompt ? prompt : "", result ? result : ""};
   (void)db1_call(&family, AIMEE_DB1_OP_AGENT_CACHE_PUT, fields, 3, NULL, NULL, 0, NULL);
}

char *db1_web_page_get(const char *url, long *age_secs, char *pinned_addr_out, size_t pinned_addr_len)
{
   if (!url || !url[0] || !age_secs || !pinned_addr_out || pinned_addr_len == 0)
      return NULL;
   const char *fields[] = {url};
   char *value = malloc(1048576u);
   if (!value)
      return NULL;
   value[0] = '\0';
   char slot1[32];
   char *const values[] = {value, slot1, pinned_addr_out};
   const size_t caps[] = {1048576u, sizeof slot1, pinned_addr_len};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WEB_PAGE_GET, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])
   {
      free(value);
      return NULL;
   }
   *age_secs = (int64_t)strtoll(slot1, NULL, 10);
   char *shrunk = realloc(value, strlen(value) + 1u);
   return shrunk ? shrunk : value;
}

int db1_web_page_put(const char *url, const char *body, const char *pinned_addr)
{
   if (!url || !url[0])
      return -1;
   const char *fields[] = {url, body ? body : "", pinned_addr ? pinned_addr : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_WEB_PAGE_PUT, fields, 3, NULL, NULL, 0, NULL));
}

void db1_web_page_drop(const char *url)
{
   if (!url || !url[0])
      return;
   const char *fields[] = {url};
   (void)db1_call(&family, AIMEE_DB1_OP_WEB_PAGE_DROP, fields, 1, NULL, NULL, 0, NULL);
}

int db1_web_page_canonical_url(const char *url, char *out, size_t out_len)
{
   if (!url || !url[0] || !out || out_len == 0)
      return -1;
   const char *fields[] = {url};
   char slot_rc[32];
   char *const values[] = {out, slot_rc};
   const size_t caps[] = {out_len, sizeof slot_rc};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_WEB_PAGE_CANONICAL_URL, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int64_t db1_fsnap_get_or_create(const char *session_id, int turn, const char *label)
{
   if (!session_id || !session_id[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", turn);
   const char *fields[] = {session_id, arg1, label ? label : ""};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_FSNAP_GET_OR_CREATE, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int64_t)strtoll(slot0, NULL, 10);
}

int db1_fsnap_record_file(int64_t snap_id, const char *path)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)snap_id);
   const char *fields[] = {arg0, path ? path : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_FSNAP_RECORD_FILE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version, int ttl_hours, db1_mcp_osv_cache_row_t *out)
{
   if (!ecosystem || !ecosystem[0] || !name || !name[0] || !version || !version[0] || !out)
      return -1;
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%d", ttl_hours);
   const char *fields[] = {ecosystem, name, version, arg3};
   char slot6[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->client_name, out->ecosystem, out->name, out->version, out->verdict, out->advisory_ids, slot6, out->checked_at_text};
   const size_t caps[] = {sizeof out->client_name, sizeof out->ecosystem, sizeof out->name, sizeof out->version, sizeof out->verdict, sizeof out->advisory_ids, sizeof slot6, sizeof out->checked_at_text};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MCP_OSV_CACHE_GET, fields, 4, values, caps, 8, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   out->checked_at = (int64_t)strtoll(slot6, NULL, 10);
   return 1;
}

int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version, const char *verdict, const char *advisory_ids)
{
   if (!ecosystem || !ecosystem[0] || !name || !name[0] || !version || !version[0])
      return -1;
   const char *fields[] = {ecosystem, name, version, verdict ? verdict : "", advisory_ids ? advisory_ids : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_MCP_OSV_CACHE_UPSERT, fields, 5, NULL, NULL, 0, NULL));
}

int db1_mcp_osv_cache_list(db1_mcp_osv_cache_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
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
      wire_values[wire_row * 8u + 0u] = out[wire_row].client_name;
      wire_caps[wire_row * 8u + 0u] = sizeof out[wire_row].client_name;
      wire_values[wire_row * 8u + 1u] = out[wire_row].ecosystem;
      wire_caps[wire_row * 8u + 1u] = sizeof out[wire_row].ecosystem;
      wire_values[wire_row * 8u + 2u] = out[wire_row].name;
      wire_caps[wire_row * 8u + 2u] = sizeof out[wire_row].name;
      wire_values[wire_row * 8u + 3u] = out[wire_row].version;
      wire_caps[wire_row * 8u + 3u] = sizeof out[wire_row].version;
      wire_values[wire_row * 8u + 4u] = out[wire_row].verdict;
      wire_caps[wire_row * 8u + 4u] = sizeof out[wire_row].verdict;
      wire_values[wire_row * 8u + 5u] = out[wire_row].advisory_ids;
      wire_caps[wire_row * 8u + 5u] = sizeof out[wire_row].advisory_ids;
      wire_values[wire_row * 8u + 6u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 8u + 6u] = sizeof wire_scratch[wire_row * 1u + 0u];
      wire_values[wire_row * 8u + 7u] = out[wire_row].checked_at_text;
      wire_caps[wire_row * 8u + 7u] = sizeof out[wire_row].checked_at_text;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MCP_OSV_CACHE_LIST, fields, 1, wire_values, wire_caps,
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
      out[wire_row].checked_at = (int64_t)strtoll(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name, const char *version, const char *verdict, const char *action, const char *advisory_ids)
{
   if (!ecosystem || !ecosystem[0] || !name || !name[0] || !version || !version[0])
      return -1;
   const char *fields[] = {client_name ? client_name : "", ecosystem, name, version, verdict ? verdict : "", action ? action : "", advisory_ids ? advisory_ids : ""};
   return write_result(db1_call(&family, AIMEE_DB1_OP_MCP_OSV_AUDIT, fields, 7, NULL, NULL, 0, NULL));
}

/* clang-format on */
