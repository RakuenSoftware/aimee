/* db1/web_page_cache.c: see web_page_cache.h. */

#include "web_page_cache.h"
#include "db1_internal.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db1_web_page_canonical_url(const char *url, char *out, size_t out_len)
{
   if (!url || !out || out_len == 0)
      return -1;

   /* scheme */
   const char *p = url;
   const char *sep = strstr(p, "://");
   if (!sep || sep - p >= 8)
      return -1;
   char scheme[8];
   size_t sl = (size_t)(sep - p);
   for (size_t i = 0; i < sl; i++)
      scheme[i] = (char)tolower((unsigned char)p[i]);
   scheme[sl] = '\0';
   if (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0)
      return -1;

   /* authority, lowercased; stop at path, query or fragment */
   const char *auth = sep + 3;
   const char *rest = auth;
   while (*rest && *rest != '/' && *rest != '?' && *rest != '#')
      rest++;
   size_t al = (size_t)(rest - auth);
   if (al == 0 || al >= 256)
      return -1;
   char host[256];
   for (size_t i = 0; i < al; i++)
      host[i] = (char)tolower((unsigned char)auth[i]);
   host[al] = '\0';

   /* suppress a default port so :443 and the bare host are one entry */
   char *colon = strrchr(host, ':');
   if (colon && !strchr(colon, ']')) /* not part of a v6 literal */
   {
      int dflt = (strcmp(scheme, "https") == 0) ? 443 : 80;
      if (atoi(colon + 1) == dflt)
         *colon = '\0';
   }

   /* path + query verbatim; fragment dropped -- it never goes on the wire, so
    * including it would store the same bytes twice */
   char tail[2048];
   size_t t = 0;
   for (const char *q = rest; *q && *q != '#' && t + 1 < sizeof(tail); q++)
      tail[t++] = *q;
   tail[t] = '\0';
   if (t == 0)
   {
      tail[0] = '/';
      tail[1] = '\0';
   }

   int n = snprintf(out, out_len, "%s://%s%s", scheme, host, tail);
   return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

char *db1_web_page_get(const char *url, long *age_secs, char *pinned_addr_out,
                       size_t pinned_addr_len)
{
   if (age_secs)
      *age_secs = 0;
   if (pinned_addr_out && pinned_addr_len)
      pinned_addr_out[0] = '\0';
   if (!url)
      return NULL;

   char key[2304];
   if (db1_web_page_canonical_url(url, key, sizeof(key)) != 0)
      return NULL;

   sqlite3 *db = db1_conn();
   if (!db)
      return NULL; /* no cache is a miss, never an error */

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT body, pinned_addr, CAST(strftime('%s','now') AS INTEGER)"
                            " - CAST(strftime('%s', fetched_at) AS INTEGER)"
                            " FROM web_page_cache WHERE url = ?"
                            " AND fetched_at > datetime('now', '-' || ? || ' seconds')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;

   char ttl[16];
   snprintf(ttl, sizeof(ttl), "%d", DB1_WEB_PAGE_TTL_SECONDS);
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, ttl, -1, SQLITE_TRANSIENT);

   char *body = NULL;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *b = sqlite3_column_text(stmt, 0);
      if (b)
         body = strdup((const char *)b);
      const unsigned char *pa = sqlite3_column_text(stmt, 1);
      if (pa && pinned_addr_out && pinned_addr_len)
         snprintf(pinned_addr_out, pinned_addr_len, "%s", (const char *)pa);
      if (age_secs)
      {
         sqlite3_int64 a = sqlite3_column_int64(stmt, 2);
         *age_secs = (long)(a < 0 ? 0 : a);
      }
   }
   sqlite3_finalize(stmt);

   if (body)
   {
      /* refresh LRU position; failure here is irrelevant to the caller */
      sqlite3_stmt *up = NULL;
      if (sqlite3_prepare_v2(db,
                             "UPDATE web_page_cache SET last_used_at = datetime('now')"
                             " WHERE url = ?",
                             -1, &up, NULL) == SQLITE_OK)
      {
         sqlite3_bind_text(up, 1, key, -1, SQLITE_TRANSIENT);
         sqlite3_step(up);
         sqlite3_finalize(up);
      }
   }
   return body;
}

/* Evict least-recently-used rows until the table is under the advisory cap.
 * Never refuses a write: a page larger than the cap is still stored, and the
 * over-cap condition is left for the caller's log rather than silently
 * disabling the cache for large pages. */
static void web_page_evict_if_needed(sqlite3 *db)
{
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(byte_len),0) FROM web_page_cache", -1, &stmt,
                          NULL) != SQLITE_OK)
      return;
   sqlite3_int64 total = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      total = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);

   while (total > (sqlite3_int64)DB1_WEB_PAGE_MAX_BYTES)
   {
      sqlite3_stmt *pick = NULL;
      if (sqlite3_prepare_v2(db,
                             "SELECT url, byte_len FROM web_page_cache"
                             " ORDER BY last_used_at ASC LIMIT 1",
                             -1, &pick, NULL) != SQLITE_OK)
         return;
      char victim[2304] = "";
      sqlite3_int64 vlen = 0;
      if (sqlite3_step(pick) == SQLITE_ROW)
      {
         const unsigned char *u = sqlite3_column_text(pick, 0);
         if (u)
            snprintf(victim, sizeof(victim), "%s", (const char *)u);
         vlen = sqlite3_column_int64(pick, 1);
      }
      sqlite3_finalize(pick);
      if (!victim[0])
         return;

      sqlite3_stmt *del = NULL;
      if (sqlite3_prepare_v2(db, "DELETE FROM web_page_cache WHERE url = ?", -1, &del, NULL) !=
          SQLITE_OK)
         return;
      sqlite3_bind_text(del, 1, victim, -1, SQLITE_TRANSIENT);
      sqlite3_step(del);
      sqlite3_finalize(del);
      total -= vlen;
      if (vlen <= 0)
         return; /* no progress possible; stop rather than spin */
   }
}

int db1_web_page_put(const char *url, const char *body, const char *pinned_addr)
{
   if (!url || !body)
      return -1;

   char key[2304];
   if (db1_web_page_canonical_url(url, key, sizeof(key)) != 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO web_page_cache (url, body, byte_len, pinned_addr, fetched_at, last_used_at)"
       " VALUES (?, ?, ?, ?, datetime('now'), datetime('now'))"
       " ON CONFLICT(url) DO UPDATE SET body = excluded.body, byte_len = excluded.byte_len,"
       " pinned_addr = excluded.pinned_addr, fetched_at = excluded.fetched_at,"
       " last_used_at = excluded.last_used_at";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, body, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, (sqlite3_int64)strlen(body));
   sqlite3_bind_text(stmt, 4, pinned_addr ? pinned_addr : "", -1, SQLITE_TRANSIENT);
   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_finalize(stmt);

   if (rc == 0)
      web_page_evict_if_needed(db);
   return rc;
}

void db1_web_page_drop(const char *url)
{
   if (!url)
      return;
   char key[2304];
   if (db1_web_page_canonical_url(url, key, sizeof(key)) != 0)
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM web_page_cache WHERE url = ?", -1, &stmt, NULL) !=
       SQLITE_OK)
      return;
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}
