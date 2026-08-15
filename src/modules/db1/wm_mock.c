/* db1/wm_mock.c: in-memory mock of wm.h for unit tests.
 *
 * Link this INSTEAD OF wm.c (never both — they define the same symbols).
 * Also define a mock db1_init.c if the test doesn't want the sqlite
 * implementation; see db1_init_mock.c.
 *
 * Deliberately simple: fixed-size array, linear scan. Enough to exercise
 * the public API contract without pulling in SQLite. */

#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOCK_WM_CAPACITY 256

static wm_entry_t g_entries[MOCK_WM_CAPACITY];
static int g_count = 0;
static int64_t g_next_id = 1;

static int expired(const wm_entry_t *e, const char *now_iso)
{
   return e->expires_at[0] && strcmp(e->expires_at, now_iso) <= 0;
}

static void now_iso(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm tm_buf;
   gmtime_r(&t, &tm_buf);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

static int find_idx(const char *session_id, const char *key)
{
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_entries[i].session_id, session_id) == 0 && strcmp(g_entries[i].key, key) == 0)
         return i;
   }
   return -1;
}

int db1_wm_set(const char *session_id, const char *key, const char *value, const char *category,
               int ttl_seconds)
{
   if (!session_id || !key || !value)
      return -1;
   if (!category || !category[0])
      category = "general";

   char ts[32];
   now_iso(ts, sizeof(ts));
   char expires[32] = {0};
   if (ttl_seconds > 0)
   {
      time_t t = time(NULL) + ttl_seconds;
      struct tm tm_buf;
      gmtime_r(&t, &tm_buf);
      strftime(expires, sizeof(expires), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   }

   int idx = find_idx(session_id, key);
   wm_entry_t *e;
   if (idx >= 0)
   {
      e = &g_entries[idx];
   }
   else
   {
      if (g_count >= MOCK_WM_CAPACITY)
         return -1;
      e = &g_entries[g_count++];
      e->id = g_next_id++;
      snprintf(e->session_id, sizeof(e->session_id), "%s", session_id);
      snprintf(e->key, sizeof(e->key), "%s", key);
      snprintf(e->created_at, sizeof(e->created_at), "%s", ts);
   }

   snprintf(e->value, sizeof(e->value), "%s", value);
   snprintf(e->category, sizeof(e->category), "%s", category);
   snprintf(e->updated_at, sizeof(e->updated_at), "%s", ts);
   snprintf(e->expires_at, sizeof(e->expires_at), "%s", expires);
   return 0;
}

int db1_wm_get(const char *session_id, const char *key, wm_entry_t *out)
{
   if (!session_id || !key || !out)
      return -1;
   int idx = find_idx(session_id, key);
   if (idx < 0)
      return -1;
   char ts[32];
   now_iso(ts, sizeof(ts));
   if (expired(&g_entries[idx], ts))
      return -1;
   *out = g_entries[idx];
   return 0;
}

int db1_wm_list(const char *session_id, const char *category, wm_entry_t *out, int max)
{
   if (!session_id || !out || max <= 0)
      return 0;
   char ts[32];
   now_iso(ts, sizeof(ts));
   int count = 0;
   for (int i = 0; i < g_count && count < max; i++)
   {
      if (strcmp(g_entries[i].session_id, session_id) != 0)
         continue;
      if (expired(&g_entries[i], ts))
         continue;
      if (category && category[0] && strcmp(g_entries[i].category, category) != 0)
         continue;
      out[count++] = g_entries[i];
   }
   return count;
}

int db1_wm_search_session_ids(const char *query, char out[][WM_SESSION_ID_LEN], int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;

   char ts[32];
   now_iso(ts, sizeof(ts));
   int count = 0;
   for (int i = g_count - 1; i >= 0 && count < max; i--)
   {
      if (expired(&g_entries[i], ts))
         continue;
      if (!strstr(g_entries[i].key, query) && !strstr(g_entries[i].value, query))
         continue;

      int duplicate = 0;
      for (int j = 0; j < count; j++)
      {
         if (strcmp(out[j], g_entries[i].session_id) == 0)
         {
            duplicate = 1;
            break;
         }
      }
      if (duplicate)
         continue;

      snprintf(out[count], WM_SESSION_ID_LEN, "%s", g_entries[i].session_id);
      count++;
   }
   return count;
}

int db1_wm_delete(const char *session_id, const char *key)
{
   if (!session_id || !key)
      return -1;
   int idx = find_idx(session_id, key);
   if (idx < 0)
      return 0;
   for (int i = idx; i < g_count - 1; i++)
      g_entries[i] = g_entries[i + 1];
   g_count--;
   return 0;
}

int db1_wm_clear(const char *session_id)
{
   if (!session_id)
      return -1;
   int w = 0;
   for (int r = 0; r < g_count; r++)
   {
      if (strcmp(g_entries[r].session_id, session_id) != 0)
         g_entries[w++] = g_entries[r];
   }
   g_count = w;
   return 0;
}

int db1_wm_gc(void)
{
   char ts[32];
   now_iso(ts, sizeof(ts));
   int w = 0;
   int removed = 0;
   for (int r = 0; r < g_count; r++)
   {
      if (expired(&g_entries[r], ts))
         removed++;
      else
         g_entries[w++] = g_entries[r];
   }
   g_count = w;
   return removed;
}

char *db1_wm_assemble_context(const char *session_id)
{
   if (!session_id)
      return NULL;
   wm_entry_t entries[WM_MAX_RESULTS];
   int count = db1_wm_list(session_id, NULL, entries, WM_MAX_RESULTS);
   if (count == 0)
      return NULL;

   size_t buf_size = 64;
   for (int i = 0; i < count; i++)
      buf_size +=
          strlen(entries[i].category) + strlen(entries[i].key) + strlen(entries[i].value) + 16;

   char *buf = malloc(buf_size);
   if (!buf)
      return NULL;
   int pos = snprintf(buf, buf_size, "## Working Memory\n");
   for (int i = 0; i < count; i++)
   {
      pos += snprintf(buf + pos, buf_size - (size_t)pos, "[%s] %s: %s\n", entries[i].category,
                      entries[i].key, entries[i].value);
   }
   return buf;
}
