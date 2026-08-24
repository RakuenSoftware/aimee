#include "headers/session_search_tool.h"
#include "util.h"
#include "db1_client/db1.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clamp_int(cJSON *args, const char *name, int def, int min, int max)
{
   cJSON *j = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
   int v = cJSON_IsNumber(j) ? (int)j->valuedouble : def;
   if (v < min)
      v = min;
   if (v > max)
      v = max;
   return v;
}

static const char *message_role(cJSON *msg)
{
   cJSON *role = cJSON_IsObject(msg) ? cJSON_GetObjectItemCaseSensitive(msg, "role") : NULL;
   return cJSON_IsString(role) ? role->valuestring : "";
}

static const char *message_text(cJSON *msg)
{
   cJSON *content = cJSON_IsObject(msg) ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
   if (cJSON_IsString(content))
      return content->valuestring;
   cJSON *text = cJSON_IsObject(msg) ? cJSON_GetObjectItemCaseSensitive(msg, "text") : NULL;
   return cJSON_IsString(text) ? text->valuestring : "";
}

static void add_message(cJSON *arr, cJSON *messages, int idx)
{
   cJSON *msg = cJSON_GetArrayItem(messages, idx);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddNumberToObject(out, "message_id", idx);
   cJSON_AddStringToObject(out, "role", message_role(msg));
   cJSON_AddStringToObject(out, "text", message_text(msg));
   cJSON_AddItemToArray(arr, out);
}

static cJSON *message_window(cJSON *messages, int center, int radius)
{
   cJSON *arr = cJSON_CreateArray();
   int n = cJSON_GetArraySize(messages);
   if (center < 0)
      center = 0;
   int start = center - radius;
   if (start < 0)
      start = 0;
   int end = center + radius;
   if (end >= n)
      end = n - 1;
   for (int i = start; i <= end; i++)
      add_message(arr, messages, i);
   return arr;
}

static cJSON *message_bookend(cJSON *messages, int from_end)
{
   cJSON *arr = cJSON_CreateArray();
   int n = cJSON_GetArraySize(messages);
   int start = from_end ? n - 3 : 0;
   if (start < 0)
      start = 0;
   int end = from_end ? n - 1 : 2;
   if (end >= n)
      end = n - 1;
   for (int i = start; i <= end; i++)
      add_message(arr, messages, i);
   return arr;
}

static int best_match_index(cJSON *messages, const char *query)
{
   int n = cJSON_GetArraySize(messages);
   for (int i = 0; i < n; i++)
   {
      if (str_contains_ci(message_text(cJSON_GetArrayItem(messages, i)), query))
         return i;
   }
   /* No message body contains the query. The DB-level search also matches on
    * session_id / agent_name / provider, and message_text() cannot read
    * structured (array) content, so a session can legitimately reach here with
    * no in-message hit. Report "no match" rather than pointing at message 0,
    * which would present an unrelated message as the search result. Callers
    * guard for -1: the snippet is emptied and the window falls back to the
    * start of the session. */
   return -1;
}

static const char *session_title(const char *session_id, char *buf, size_t cap)
{
   buf[0] = '\0';
   db1_server_session_t ss;
   if (db1_server_session_get(session_id, &ss) == 0 && ss.title[0])
      snprintf(buf, cap, "%s", ss.title);
   return buf;
}

static int source_included(const char *source, cJSON *include_sources)
{
   if (!source || !source[0])
      return 1;
   if (strcmp(source, "mcp") != 0 && strcmp(source, "tool") != 0 &&
       strcmp(source, "internal") != 0 && strcmp(source, "delegate") != 0)
      return 1;
   if (!cJSON_IsArray(include_sources))
      return 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, include_sources)
   {
      if (cJSON_IsString(item) && strcmp(item->valuestring, source) == 0)
         return 1;
   }
   return 0;
}

static int row_allowed(const db1_primary_session_row_t *row, cJSON *include_sources,
                       const char *principal)
{
   db1_server_session_t ss;
   if (db1_server_session_get(row->session_id, &ss) != 0)
      return 1;
   if (principal && principal[0] && ss.principal[0] && strcmp(ss.principal, principal) != 0)
      return 0;
   return source_included(ss.client_type, include_sources);
}

static void add_session_metadata(cJSON *obj, const db1_primary_session_row_t *row)
{
   char title[DB1_SS_TITLE_LEN];
   db1_server_session_t ss;
   cJSON_AddStringToObject(obj, "session_id", row->session_id);
   cJSON_AddStringToObject(obj, "agent_name", row->agent_name);
   cJSON_AddStringToObject(obj, "provider", row->provider);
   cJSON_AddStringToObject(obj, "title", session_title(row->session_id, title, sizeof(title)));
   if (db1_server_session_get(row->session_id, &ss) == 0)
      cJSON_AddStringToObject(obj, "source", ss.client_type);
   else
      cJSON_AddStringToObject(obj, "source", "");
   cJSON_AddStringToObject(obj, "started_at", row->created_at);
   cJSON_AddStringToObject(obj, "updated_at", row->updated_at);
}

static void add_preview(cJSON *obj, cJSON *messages)
{
   int n = cJSON_GetArraySize(messages);
   for (int i = 0; i < n; i++)
   {
      const char *text = message_text(cJSON_GetArrayItem(messages, i));
      if (text && text[0])
      {
         cJSON_AddStringToObject(obj, "preview", text);
         return;
      }
   }
   cJSON_AddStringToObject(obj, "preview", "");
}

static void add_discovery_session(cJSON *sessions, const db1_primary_session_row_t *row,
                                  const char *query, int window)
{
   cJSON *messages = cJSON_Parse(row->messages_json ? row->messages_json : "[]");
   if (!cJSON_IsArray(messages))
   {
      cJSON_Delete(messages);
      messages = cJSON_CreateArray();
   }
   int match_idx = best_match_index(messages, query);

   cJSON *obj = cJSON_CreateObject();
   add_session_metadata(obj, row);
   cJSON *match = cJSON_CreateObject();
   cJSON_AddNumberToObject(match, "message_id", match_idx);
   cJSON_AddStringToObject(match, "snippet",
                           match_idx >= 0 ? message_text(cJSON_GetArrayItem(messages, match_idx))
                                          : "");
   cJSON_AddItemToObject(obj, "match", match);
   cJSON_AddItemToObject(obj, "window", message_window(messages, match_idx, window));
   cJSON_AddItemToObject(obj, "bookend_start", message_bookend(messages, 0));
   cJSON_AddItemToObject(obj, "bookend_end", message_bookend(messages, 1));
   cJSON_AddItemToArray(sessions, obj);
   cJSON_Delete(messages);
}

static cJSON *session_search_discovery(cJSON *args, const char *query)
{
   int limit = clamp_int(args, "limit", 5, 1, 20);
   int window = clamp_int(args, "window", 5, 0, 20);
   cJSON *include_sources = cJSON_GetObjectItemCaseSensitive(args, "include_sources");
   cJSON *principal = cJSON_GetObjectItemCaseSensitive(args, "_principal");
   const char *principal_value = cJSON_IsString(principal) ? principal->valuestring : NULL;
   db1_primary_session_row_t *rows = NULL;
   int fetch_limit = limit * 4;
   int n = db1_primary_session_alloc_search(query, &rows, fetch_limit);

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "mode", "discovery");
   cJSON_AddStringToObject(root, "query", query);
   cJSON *sessions = cJSON_AddArrayToObject(root, "sessions");
   int visible = 0;
   if (n > 0)
   {
      for (int i = 0; i < n && visible < limit; i++)
      {
         if (!row_allowed(&rows[i], include_sources, principal_value))
            continue;
         add_discovery_session(sessions, &rows[i], query, window);
         visible++;
      }
   }
   cJSON_AddNumberToObject(root, "count", visible);
   db1_primary_session_rows_free(rows, n > 0 ? n : 0);
   return root;
}

static cJSON *session_search_browse(cJSON *args)
{
   int limit = clamp_int(args, "limit", 5, 1, 20);
   cJSON *include_sources = cJSON_GetObjectItemCaseSensitive(args, "include_sources");
   cJSON *principal = cJSON_GetObjectItemCaseSensitive(args, "_principal");
   const char *principal_value = cJSON_IsString(principal) ? principal->valuestring : NULL;
   db1_primary_session_row_t *rows = NULL;
   int fetch_limit = limit * 4;
   int n = db1_primary_session_alloc_recent(&rows, fetch_limit);

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "mode", "browse");
   cJSON *sessions = cJSON_AddArrayToObject(root, "sessions");
   int visible = 0;
   if (n > 0)
   {
      for (int i = 0; i < n && visible < limit; i++)
      {
         if (!row_allowed(&rows[i], include_sources, principal_value))
            continue;
         cJSON *messages = cJSON_Parse(rows[i].messages_json ? rows[i].messages_json : "[]");
         if (!cJSON_IsArray(messages))
         {
            cJSON_Delete(messages);
            messages = cJSON_CreateArray();
         }
         cJSON *obj = cJSON_CreateObject();
         add_session_metadata(obj, &rows[i]);
         add_preview(obj, messages);
         cJSON_AddNumberToObject(obj, "message_count", cJSON_GetArraySize(messages));
         cJSON_AddItemToArray(sessions, obj);
         cJSON_Delete(messages);
         visible++;
      }
   }
   cJSON_AddNumberToObject(root, "count", visible);
   db1_primary_session_rows_free(rows, n > 0 ? n : 0);
   return root;
}

static cJSON *session_search_scroll(cJSON *args, const char *session_id, int around_message_id)
{
   int window = clamp_int(args, "window", 5, 0, 20);
   db1_primary_session_row_t row;
   int hit = db1_primary_session_get_latest(session_id, &row);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "mode", "scroll");
   cJSON_AddStringToObject(root, "session_id", session_id);
   if (hit != 0)
   {
      cJSON_AddBoolToObject(root, "found", 0);
      cJSON_AddItemToObject(root, "window", cJSON_CreateArray());
      return root;
   }
   cJSON *include_sources = cJSON_GetObjectItemCaseSensitive(args, "include_sources");
   cJSON *principal = cJSON_GetObjectItemCaseSensitive(args, "_principal");
   const char *principal_value = cJSON_IsString(principal) ? principal->valuestring : NULL;
   if (!row_allowed(&row, include_sources, principal_value))
   {
      cJSON_AddBoolToObject(root, "found", 0);
      cJSON_AddItemToObject(root, "window", cJSON_CreateArray());
      db1_primary_session_row_clear(&row);
      return root;
   }

   cJSON *messages = cJSON_Parse(row.messages_json ? row.messages_json : "[]");
   if (!cJSON_IsArray(messages))
   {
      cJSON_Delete(messages);
      messages = cJSON_CreateArray();
   }
   cJSON_AddBoolToObject(root, "found", 1);
   cJSON_AddStringToObject(root, "agent_name", row.agent_name);
   cJSON_AddStringToObject(root, "provider", row.provider);
   cJSON_AddNumberToObject(root, "around_message_id", around_message_id);
   cJSON_AddItemToObject(root, "window", message_window(messages, around_message_id, window));
   cJSON_AddNumberToObject(root, "message_count", cJSON_GetArraySize(messages));
   cJSON_Delete(messages);
   db1_primary_session_row_clear(&row);
   return root;
}

cJSON *session_search_tool_result(cJSON *args)
{
   cJSON *query = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "query") : NULL;
   if (cJSON_IsString(query) && query->valuestring[0])
      return session_search_discovery(args, query->valuestring);

   cJSON *sid = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "session_id") : NULL;
   cJSON *around =
       cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "around_message_id") : NULL;
   if (cJSON_IsString(sid) && sid->valuestring[0] && cJSON_IsNumber(around))
      return session_search_scroll(args, sid->valuestring, (int)around->valuedouble);

   return session_search_browse(args);
}

cJSON *session_search_tool_result_for_uid(cJSON *args, unsigned uid)
{
   cJSON *search_args = cJSON_IsObject(args) ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
   if (search_args)
   {
      char principal[32];
      snprintf(principal, sizeof(principal), "uid:%u", uid);
      cJSON_AddStringToObject(search_args, "_principal", principal);
   }
   cJSON *result = session_search_tool_result(search_args ? search_args : args);
   cJSON_Delete(search_args);
   return result;
}

cJSON *session_search_mcp_tool(void)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", "session_search");
   cJSON_AddStringToObject(t, "description", "Search stored primary-agent session transcripts.");
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"session_"
                   "id\":{\"type\":\"string\"},\"around_message_id\":{\"type\":\"integer\"},"
                   "\"window\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"},\"include_"
                   "sources\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}");
   cJSON_AddItemToObject(t, "inputSchema", schema ? schema : cJSON_CreateObject());
   return t;
}
