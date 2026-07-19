/* conversation_context.c: session-local tool-chain stub generation (Phase 1).
 *
 * Feature gate: session.virtual_context.enabled must be true (default: false).
 * All functions are no-ops when the flag is off.
 *
 * Chain grouping strategy (Phase 1):
 *   Groups of up to CHAIN_MAX_EVENTS consecutive pending events are flushed
 *   into a single chain with a deterministic stub.  Auto-flush triggers when
 *   the pending event count reaches AUTO_FLUSH_THRESHOLD.
 */
#include "headers/conversation_context.h"
#include "db1/conv_context.h"
#include "config.h"
#include "headers/log.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#define CHAIN_MAX_EVENTS     5
#define AUTO_FLUSH_THRESHOLD 10
#define STUB_EXCERPT_LEN     180
#define STUB_TOOLS_MAX       256
#define STUB_FILES_MAX       256
#define STUB_BUF_SIZE        2048
#define MAX_PENDING          64
#define MAX_CHAINS           64

static int virtual_context_on(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 0;
   return cfg.virtual_context_enabled;
}

/* Extract a JSON string field from a JSON object string. */
static void extract_json_field(const char *json_str, const char *field, char *out, int out_len)
{
   if (!json_str || !json_str[0])
      return;
   cJSON *j = cJSON_Parse(json_str);
   if (!j)
      return;
   cJSON *v = cJSON_GetObjectItemCaseSensitive(j, field);
   if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
      snprintf(out, (size_t)out_len, "%s", v->valuestring);
   cJSON_Delete(j);
}

/* Append a token to a comma-separated list if not already present. */
static void append_unique(char *buf, int buf_len, const char *token)
{
   if (!token || !token[0])
      return;
   /* check for exact token already in buf */
   char search[256];
   snprintf(search, sizeof(search), "%s", token);
   if (strstr(buf, search))
      return;
   int cur = (int)strlen(buf);
   if (cur > 0 && cur < buf_len - 2)
   {
      buf[cur++] = ',';
      buf[cur] = '\0';
   }
   snprintf(buf + cur, (size_t)(buf_len - cur), "%s", token);
}

/* Build a deterministic stub from a slice of tool events. */
static void build_stub(const conv_tool_event_t *events, int count, char *out, int out_len,
                       int *raw_bytes_out, int *stub_bytes_out)
{
   char tools[STUB_TOOLS_MAX] = "";
   char files[STUB_FILES_MAX] = "";
   char excerpt[STUB_EXCERPT_LEN + 1] = "";
   int total_raw = 0;
   int has_excerpt = 0;

   for (int i = 0; i < count; i++)
   {
      const conv_tool_event_t *e = &events[i];
      total_raw += e->result_bytes > 0 ? e->result_bytes : (int)strlen(e->tool_result);

      append_unique(tools, sizeof(tools), e->tool_name);

      /* Extract file_path / path from tool_input */
      char fpath[256] = "";
      extract_json_field(e->tool_input, "file_path", fpath, sizeof(fpath));
      if (!fpath[0])
         extract_json_field(e->tool_input, "path", fpath, sizeof(fpath));
      if (fpath[0])
         append_unique(files, sizeof(files), fpath);

      /* Keep the first result excerpt */
      if (!has_excerpt && e->tool_result[0])
      {
         snprintf(excerpt, sizeof(excerpt), "%.*s", STUB_EXCERPT_LEN, e->tool_result);
         has_excerpt = 1;
      }
   }

   int pos = 0;
   if (tools[0])
      pos += snprintf(out + pos, (size_t)(out_len - pos), "Tools: %s.", tools);
   if (files[0] && pos < out_len - 2)
      pos += snprintf(out + pos, (size_t)(out_len - pos), " Files: %s.", files);
   if (has_excerpt && pos < out_len - 2)
   {
      /* strip newlines for compactness */
      for (int k = 0; excerpt[k]; k++)
         if (excerpt[k] == '\n' || excerpt[k] == '\r')
            excerpt[k] = ' ';
      pos += snprintf(out + pos, (size_t)(out_len - pos), " Excerpt: %s", excerpt);
   }

   *raw_bytes_out = total_raw;
   *stub_bytes_out = pos;
}

int64_t conv_ctx_record_event(const char *sid, const char *tool_name, const char *tool_input,
                              const char *tool_result, int result_bytes)
{
   if (!virtual_context_on())
      return 0;
   if (!sid || !tool_name)
      return -1;

   int64_t id = db1_conv_record_event(sid, tool_name, tool_input, tool_result, result_bytes);
   if (id <= 0)
      return id;

   /* Auto-flush: count pending events; trigger if threshold reached */
   conv_tool_event_t peek[1];
   int pending = db1_conv_pending_events(sid, peek, 1);
   /* We just inserted one; if we had pending already, re-check */
   (void)pending; /* suppress unused-variable warning */

   /* Use state to track count cheaply */
   int64_t last_event = 0;
   int chain_count = 0, event_count = 0;
   db1_conv_state_get(sid, &last_event, &chain_count, &event_count);
   event_count++;
   db1_conv_state_update(sid, id, chain_count, event_count);

   /* Auto-flush when we have enough events pending */
   conv_tool_event_t probe[AUTO_FLUSH_THRESHOLD + 1];
   int n = db1_conv_pending_events(sid, probe, AUTO_FLUSH_THRESHOLD + 1);
   if (n >= AUTO_FLUSH_THRESHOLD)
      conv_ctx_flush_pending(sid);

   return id;
}

int conv_ctx_flush_pending(const char *sid)
{
   if (!sid)
      return -1;

   conv_tool_event_t events[MAX_PENDING];
   int n = db1_conv_pending_events(sid, events, MAX_PENDING);
   if (n <= 0)
      return 0;

   int chains_created = 0;
   int i = 0;
   while (i < n)
   {
      int batch = n - i;
      if (batch > CHAIN_MAX_EVENTS)
         batch = CHAIN_MAX_EVENTS;
      if (batch < 2)
         break; /* leave fewer than 2 events pending */

      const conv_tool_event_t *slice = &events[i];
      int64_t first = slice[0].id;
      int64_t last = slice[batch - 1].id;

      /* Build tool list for chain record */
      char tools[STUB_TOOLS_MAX] = "";
      for (int k = 0; k < batch; k++)
         append_unique(tools, sizeof(tools), slice[k].tool_name);

      char stub[STUB_BUF_SIZE] = "";
      int raw_bytes = 0, stub_bytes = 0;
      build_stub(slice, batch, stub, sizeof(stub), &raw_bytes, &stub_bytes);

      int64_t chain_id =
          db1_conv_insert_chain(sid, first, last, tools, stub, raw_bytes, stub_bytes);
      if (chain_id <= 0)
      {
         LOG_WARN("conv_ctx", "flush: insert_chain failed for %s", sid);
         break;
      }

      db1_conv_set_chain_id(first, last, chain_id);
      chains_created++;

      /* Update state */
      int64_t last_ev = 0;
      int cc = 0, ec = 0;
      db1_conv_state_get(sid, &last_ev, &cc, &ec);
      cc++;
      db1_conv_state_update(sid, last_ev, cc, ec);

      i += batch;
   }
   return chains_created;
}

/* ---- Phase 2: budget-aware assembly ---- */

#define ASSEMBLE_DEFAULT_BUDGET 4096
#define ASSEMBLE_MAX_CHAINS     32
#define ASSEMBLE_LINE_MAX       256

char *conv_ctx_assemble(const char *sid, const char *query, int budget_bytes)
{
   if (!virtual_context_on())
      return NULL;
   if (!sid || !sid[0])
      return NULL;

   config_t cfg;
   if (config_load(&cfg) == 0 && budget_bytes <= 0)
      budget_bytes = cfg.virtual_context_assembly_budget > 0 ? cfg.virtual_context_assembly_budget
                                                             : ASSEMBLE_DEFAULT_BUDGET;
   if (budget_bytes <= 0)
      budget_bytes = ASSEMBLE_DEFAULT_BUDGET;

   /* Flush any pending events first */
   conv_ctx_flush_pending(sid);

   conv_tool_chain_t chains[ASSEMBLE_MAX_CHAINS];
   int n;
   if (query && query[0])
      n = db1_conv_search_chains(sid, query, chains, ASSEMBLE_MAX_CHAINS);
   else
      n = db1_conv_list_chains(sid, chains, ASSEMBLE_MAX_CHAINS);

   if (n <= 0)
      return NULL;

   /* Allocate output buffer */
   char *buf = malloc((size_t)budget_bytes + 1);
   if (!buf)
      return NULL;
   int pos = 0;

   pos += snprintf(buf + pos, (size_t)(budget_bytes - pos), "# Session Activity\n");

   for (int i = 0; i < n && pos < budget_bytes - ASSEMBLE_LINE_MAX; i++)
   {
      const conv_tool_chain_t *c = &chains[i];
      int written = snprintf(buf + pos, (size_t)(budget_bytes - pos), "- [chain %lld] %s\n",
                             (long long)c->id, c->stub[0] ? c->stub : c->tools);
      if (written <= 0 || pos + written >= budget_bytes - ASSEMBLE_LINE_MAX)
         break;
      pos += written;
   }

   if (pos <= (int)sizeof("# Session Activity\n") - 1)
   {
      free(buf);
      return NULL;
   }

   buf[pos] = '\0';
   return buf;
}

/* ---- MCP tool handlers ---- */

cJSON *tool_session_context_search(cJSON *args)
{
   const char *query = NULL;
   int limit = 10;
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (cJSON_IsString(jq))
      query = jq->valuestring;
   if (cJSON_IsNumber(jl))
      limit = (int)jl->valuedouble;
   if (limit <= 0 || limit > 50)
      limit = 10;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.virtual_context_enabled)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error", "virtual_context not enabled");
      return r;
   }

   if (!query || !query[0])
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error", "query is required");
      return r;
   }

   const char *sid = session_id();
   conv_tool_chain_t chains[50];
   int n = db1_conv_search_chains(sid, query, chains, limit);

   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "ok", 1);
   cJSON_AddStringToObject(r, "session_id", sid ? sid : "");
   cJSON_AddNumberToObject(r, "count", n);
   cJSON *arr = cJSON_AddArrayToObject(r, "chains");
   for (int i = 0; i < n; i++)
   {
      cJSON *c = cJSON_CreateObject();
      cJSON_AddNumberToObject(c, "id", (double)chains[i].id);
      cJSON_AddStringToObject(c, "tools", chains[i].tools);
      cJSON_AddStringToObject(c, "stub", chains[i].stub);
      cJSON_AddNumberToObject(c, "raw_bytes", chains[i].raw_bytes);
      cJSON_AddNumberToObject(c, "stub_bytes", chains[i].stub_bytes);
      cJSON_AddStringToObject(c, "state", chains[i].state);
      cJSON_AddStringToObject(c, "created_at", chains[i].created_at);
      cJSON_AddItemToArray(arr, c);
   }
   return r;
}

cJSON *tool_session_context_expand(cJSON *args)
{
   int64_t chain_id = 0;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "chain_id");
   if (cJSON_IsNumber(jid))
      chain_id = (int64_t)jid->valuedouble;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.virtual_context_enabled)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error", "virtual_context not enabled");
      return r;
   }

   if (chain_id <= 0)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error", "chain_id is required");
      return r;
   }

   conv_tool_event_t events[CHAIN_MAX_EVENTS * 4];
   int n = db1_conv_chain_events(chain_id, events, (int)(sizeof(events) / sizeof(events[0])));

   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "ok", 1);
   cJSON_AddNumberToObject(r, "chain_id", (double)chain_id);
   cJSON_AddNumberToObject(r, "event_count", n);
   cJSON *arr = cJSON_AddArrayToObject(r, "events");
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddNumberToObject(e, "id", (double)events[i].id);
      cJSON_AddStringToObject(e, "tool_name", events[i].tool_name);
      cJSON_AddStringToObject(e, "tool_input", events[i].tool_input);
      cJSON_AddStringToObject(e, "tool_result", events[i].tool_result);
      cJSON_AddNumberToObject(e, "result_bytes", events[i].result_bytes);
      cJSON_AddStringToObject(e, "created_at", events[i].created_at);
      cJSON_AddItemToArray(arr, e);
   }
   return r;
}

cJSON *tool_session_context_status(cJSON *args)
{
   (void)args;

   config_t cfg;
   config_load(&cfg);

   const char *sid = session_id();
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "session_id", sid ? sid : "");
   cJSON_AddBoolToObject(r, "enabled", cfg.virtual_context_enabled ? 1 : 0);

   if (!cfg.virtual_context_enabled)
      return r;

   /* Flush any auto-flushable pending events first */
   conv_ctx_flush_pending(sid);

   int64_t last_event = 0;
   int chain_count = 0, event_count = 0;
   db1_conv_state_get(sid, &last_event, &chain_count, &event_count);

   /* Count actually pending (not yet chained) */
   conv_tool_event_t probe[MAX_PENDING];
   int pending = db1_conv_pending_events(sid, probe, MAX_PENDING);

   cJSON_AddNumberToObject(r, "event_count", event_count);
   cJSON_AddNumberToObject(r, "chain_count", chain_count);
   cJSON_AddNumberToObject(r, "pending_events", pending);
   cJSON_AddNumberToObject(r, "last_event_id", (double)last_event);

   /* Operational metrics for the rollout dashboard, derived from the session's
    * chains.  Names mirror the rollout-validation proposal's observability set
    * (see docs/observability/virtual-context-alerts.md). */
   conv_tool_chain_t chains[MAX_CHAINS];
   int nc = db1_conv_list_chains(sid, chains, MAX_CHAINS);
   long raw_total = 0, stub_total = 0;
   for (int i = 0; i < nc; i++)
   {
      raw_total += chains[i].raw_bytes;
      stub_total += chains[i].stub_bytes;
   }
   long bytes_saved = raw_total - stub_total;
   double ratio = stub_total > 0 ? (double)raw_total / (double)stub_total : 0.0;

   cJSON *m = cJSON_AddObjectToObject(r, "metrics");
   cJSON_AddNumberToObject(m, "session_context_segments_total", nc);
   cJSON_AddNumberToObject(m, "session_tool_chains_stubbed_total", nc);
   cJSON_AddNumberToObject(m, "raw_bytes_total", (double)raw_total);
   cJSON_AddNumberToObject(m, "stub_bytes_total", (double)stub_total);
   cJSON_AddNumberToObject(m, "session_context_bytes_saved", (double)bytes_saved);
   cJSON_AddNumberToObject(m, "compression_ratio", ratio);
   return r;
}
