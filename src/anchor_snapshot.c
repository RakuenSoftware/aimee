/* anchor_snapshot.c: hashline edit core — line anchors + immutable read
 * snapshots. (proposal: hashline-edit-and-lean-websearch, Part I.)
 *
 * Pure C, no LLM, no I/O beyond an in-process TTL/LRU store guarded by a mutex.
 * The digest is FNV-1a 64-bit over a line's CANONICAL bytes (terminator and a
 * line-1 BOM excluded); the whole-file identity is FNV over the per-line
 * digests. The 2-hex display tag is a glance aid only — verification uses the
 * full 64-bit digest. */
#include "anchor_snapshot.h"
#include "dstr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- FNV-1a 64-bit ---- */

#define FNV64_OFFSET 1469598103934665603ULL
#define FNV64_PRIME  1099511628211ULL

static uint64_t fnv1a64(const void *data, size_t len)
{
   const unsigned char *p = (const unsigned char *)data;
   uint64_t h = FNV64_OFFSET;
   for (size_t i = 0; i < len; i++)
   {
      h ^= (uint64_t)p[i];
      h *= FNV64_PRIME;
   }
   return h;
}

/* Strip the trailing terminator ("\n" or "\r\n") from a raw line span, and, on
 * line 1, a leading UTF-8 BOM. Returns the canonical [start,len) to hash. */
static void canonicalize_line(const char *line, size_t len, int is_first_line,
                              const char **start_out, size_t *len_out)
{
   const char *start = line;
   /* drop terminator */
   if (len > 0 && line[len - 1] == '\n')
   {
      len--;
      if (len > 0 && line[len - 1] == '\r')
         len--;
   }
   /* drop a leading BOM on the first line only */
   if (is_first_line && len >= 3 && (unsigned char)start[0] == 0xEF &&
       (unsigned char)start[1] == 0xBB && (unsigned char)start[2] == 0xBF)
   {
      start += 3;
      len -= 3;
   }
   *start_out = start;
   *len_out = len;
}

uint64_t anchor_line_digest(const char *line, size_t len, int is_first_line)
{
   const char *start;
   size_t clen;
   canonicalize_line(line, len, is_first_line, &start, &clen);
   return fnv1a64(start, clen);
}

void anchor_short_tag(uint64_t digest, char out[3])
{
   /* low byte, 2 hex chars — a stable, glanceable token */
   snprintf(out, 3, "%02x", (unsigned)(digest & 0xffu));
}

int anchor_split_lines(const char *bytes, size_t len, anchor_line_t **out)
{
   *out = NULL;
   if (!bytes || len == 0)
      return 0;

   /* first pass: count lines (a run ended by '\n', plus a trailing partial) */
   int count = 0;
   for (size_t i = 0; i < len; i++)
      if (bytes[i] == '\n')
         count++;
   if (len > 0 && bytes[len - 1] != '\n')
      count++; /* trailing line with no newline */
   if (count == 0)
      return 0;

   anchor_line_t *lines = calloc((size_t)count, sizeof(*lines));
   if (!lines)
      return -1;

   int idx = 0;
   size_t start = 0;
   for (size_t i = 0; i < len; i++)
   {
      if (bytes[i] == '\n')
      {
         size_t line_len = i - start + 1; /* include '\n' */
         size_t content = line_len - 1;
         if (content > 0 && bytes[start + content - 1] == '\r')
            content--;
         lines[idx].ptr = bytes + start;
         lines[idx].len = line_len;
         lines[idx].content_len = content;
         idx++;
         start = i + 1;
      }
   }
   if (start < len)
   {
      /* trailing line, no terminator */
      size_t line_len = len - start;
      lines[idx].ptr = bytes + start;
      lines[idx].len = line_len;
      lines[idx].content_len = line_len;
      idx++;
   }
   *out = lines;
   return idx;
}

void anchor_detect_shape(const char *bytes, size_t len, char eol_out[3], int *had_bom,
                         int *no_final_newline)
{
   int bom = 0;
   if (len >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
       (unsigned char)bytes[2] == 0xBF)
      bom = 1;
   if (had_bom)
      *had_bom = bom;

   long crlf = 0, lf = 0;
   for (size_t i = 0; i < len; i++)
   {
      if (bytes[i] == '\n')
      {
         if (i > 0 && bytes[i - 1] == '\r')
            crlf++;
         else
            lf++;
      }
   }
   /* strict majority CRLF -> "\r\n"; tie or LF-majority -> "\n" */
   if (crlf > lf)
      memcpy(eol_out, "\r\n", 3);
   else
      memcpy(eol_out, "\n", 2);

   if (no_final_newline)
      *no_final_newline = (len == 0 || bytes[len - 1] != '\n') ? 1 : 0;
}

/* ---- snapshot store: fixed-slot TTL/LRU, mutex-guarded ---- */

#define ANCHOR_STORE_SLOTS  128
#define ANCHOR_SNAPSHOT_TTL 1800 /* seconds */

static pthread_mutex_t g_store_lock = PTHREAD_MUTEX_INITIALIZER;
static anchor_snapshot_t g_store[ANCHOR_STORE_SLOTS];
static int g_store_used[ANCHOR_STORE_SLOTS];
static unsigned long g_seq;

static void slot_free(int i)
{
   free(g_store[i].line_digests);
   memset(&g_store[i], 0, sizeof(g_store[i]));
   g_store_used[i] = 0;
}

/* Caller holds g_store_lock. Returns a slot index, evicting the oldest if the
 * store is full or a slot is expired. */
static int store_alloc_slot_locked(time_t now)
{
   int oldest = -1;
   time_t oldest_t = 0;
   for (int i = 0; i < ANCHOR_STORE_SLOTS; i++)
   {
      if (!g_store_used[i])
         return i;
      if (now - g_store[i].created >= ANCHOR_SNAPSHOT_TTL)
      {
         slot_free(i);
         return i;
      }
      if (oldest < 0 || g_store[i].created < oldest_t)
      {
         oldest = i;
         oldest_t = g_store[i].created;
      }
   }
   slot_free(oldest);
   return oldest;
}

int anchor_snapshot_create(const char *abs_path, const char *bytes, size_t len,
                           char id_out[ANCHOR_SNAPSHOT_ID_MAX])
{
   anchor_line_t *lines = NULL;
   int n = anchor_split_lines(bytes, len, &lines);
   if (n < 0)
      return -1;

   uint64_t *digests = NULL;
   if (n > 0)
   {
      digests = malloc((size_t)n * sizeof(*digests));
      if (!digests)
      {
         free(lines);
         return -1;
      }
      for (int i = 0; i < n; i++)
         digests[i] = anchor_line_digest(lines[i].ptr, lines[i].len, i == 0);
   }
   free(lines);

   uint64_t file_hash = (n > 0) ? fnv1a64(digests, (size_t)n * sizeof(*digests)) : FNV64_OFFSET;

   char eol[3];
   int had_bom = 0, no_final_newline = 0;
   anchor_detect_shape(bytes, len, eol, &had_bom, &no_final_newline);

   time_t now = time(NULL);

   pthread_mutex_lock(&g_store_lock);
   int slot = store_alloc_slot_locked(now);
   unsigned long seq = ++g_seq;
   anchor_snapshot_t *s = &g_store[slot];
   snprintf(s->id, sizeof(s->id), "s%08lx%08x", seq, (unsigned)(file_hash & 0xffffffffu));
   snprintf(s->path, sizeof(s->path), "%s", abs_path ? abs_path : "");
   s->file_hash = file_hash;
   s->line_digests = digests; /* store owns it */
   s->line_count = n;
   memcpy(s->eol, eol, 3);
   s->had_bom = had_bom;
   s->no_final_newline = no_final_newline;
   s->created = now;
   g_store_used[slot] = 1;
   snprintf(id_out, ANCHOR_SNAPSHOT_ID_MAX, "%s", s->id);
   pthread_mutex_unlock(&g_store_lock);
   return 0;
}

int anchor_snapshot_get_copy(const char *id, anchor_snapshot_t *out)
{
   if (!id || !id[0] || !out)
      return 0;
   time_t now = time(NULL);
   pthread_mutex_lock(&g_store_lock);
   for (int i = 0; i < ANCHOR_STORE_SLOTS; i++)
   {
      if (!g_store_used[i] || strcmp(g_store[i].id, id) != 0)
         continue;
      if (now - g_store[i].created >= ANCHOR_SNAPSHOT_TTL)
      {
         slot_free(i);
         break;
      }
      *out = g_store[i];
      out->line_digests = NULL;
      if (g_store[i].line_count > 0)
      {
         out->line_digests = malloc((size_t)g_store[i].line_count * sizeof(uint64_t));
         if (!out->line_digests)
         {
            pthread_mutex_unlock(&g_store_lock);
            return 0;
         }
         memcpy(out->line_digests, g_store[i].line_digests,
                (size_t)g_store[i].line_count * sizeof(uint64_t));
      }
      pthread_mutex_unlock(&g_store_lock);
      return 1;
   }
   pthread_mutex_unlock(&g_store_lock);
   return 0;
}

void anchor_snapshot_dispose(anchor_snapshot_t *out)
{
   if (!out)
      return;
   free(out->line_digests);
   out->line_digests = NULL;
}

void anchor_snapshot_gc(void)
{
   time_t now = time(NULL);
   pthread_mutex_lock(&g_store_lock);
   for (int i = 0; i < ANCHOR_STORE_SLOTS; i++)
      if (g_store_used[i] && now - g_store[i].created >= ANCHOR_SNAPSHOT_TTL)
         slot_free(i);
   pthread_mutex_unlock(&g_store_lock);
}

char *anchor_format_read(const char *bytes, size_t len, int offset, int limit,
                         const char *snapshot_id)
{
   anchor_line_t *lines = NULL;
   int n = anchor_split_lines(bytes, len, &lines);
   if (n < 0)
      return NULL;

   dstr_t ds;
   dstr_init(&ds);
   if (snapshot_id && snapshot_id[0])
   {
      dstr_append_str(&ds, "# anchored read snapshot=");
      dstr_append_str(&ds, snapshot_id);
      dstr_append_str(&ds, " (edit by anchor: pass snapshot_id + edits[{op,at,text}]; \"LINE:HH\" "
                           "cites a line)\n");
   }

   int start = (offset > 0) ? offset : 0; /* skip `offset` lines */
   int shown = 0;
   int max = (limit > 0) ? limit : n;
   for (int i = start; i < n && shown < max; i++, shown++)
   {
      uint64_t d = anchor_line_digest(lines[i].ptr, lines[i].len, i == 0);
      char tag[3];
      anchor_short_tag(d, tag);
      char prefix[32];
      snprintf(prefix, sizeof(prefix), "%d:%s| ", i + 1, tag);
      dstr_append_str(&ds, prefix);
      dstr_append(&ds, lines[i].ptr, lines[i].content_len);
      dstr_append_str(&ds, "\n");
   }
   free(lines);
   char *out = dstr_steal(&ds);
   if (!out)
   {
      /* empty window (e.g. offset past EOF, no header) — return "" not NULL */
      dstr_free(&ds);
      out = calloc(1, 1);
   }
   return out;
}

int anchor_parse(const char *token, int *ordinal, unsigned *tag)
{
   if (!token || !token[0])
      return -1;
   char *end = NULL;
   long ord = strtol(token, &end, 10);
   if (end == token || *end != ':' || ord <= 0)
      return -1;
   const char *hex = end + 1;
   if (!hex[0])
      return -1;
   char *hend = NULL;
   unsigned long t = strtoul(hex, &hend, 16);
   if (hend == hex || *hend != '\0')
      return -1;
   if (ordinal)
      *ordinal = (int)ord;
   if (tag)
      *tag = (unsigned)t;
   return 0;
}
