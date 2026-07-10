/* hashline_anchor.c: composite line anchors + immutable read snapshots.
 * See hashline_anchor.h for the contract. Leaf module: libc + sketch + util +
 * platform_random only. */
#include "hashline_anchor.h"

#include "platform_random.h"
#include "sketch.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- line canonicalization + digests ---- */

void hashline_canonicalize_line(const char *line, size_t len, int is_first_line, int had_terminator,
                                const char **out_ptr, size_t *out_len)
{
   const char *p = line;
   /* Strip a leading UTF-8 BOM only on physical line 1 (file byte 0). */
   if (is_first_line && len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
       (unsigned char)p[2] == 0xBF)
   {
      p += 3;
      len -= 3;
   }
   /* Drop the CR of a CRLF terminator so a CRLF file and an LF file with
    * identical text hash the same — but ONLY when a '\n' terminator followed this
    * line. A trailing '\r' with no terminator (e.g. the last line of the file) is
    * genuine content and is hashed verbatim, so `foo` and `foo\r` never collide. */
   if (had_terminator && len > 0 && p[len - 1] == '\r')
      len -= 1;
   *out_ptr = p;
   *out_len = len;
}

uint64_t hashline_digest64(const char *line, size_t len, int is_first_line, int had_terminator)
{
   const char *cp = NULL;
   size_t cl = 0;
   hashline_canonicalize_line(line, len, is_first_line, had_terminator, &cp, &cl);
   return sketch_fnv1a(cp, cl);
}

uint64_t hashline_digest64_raw(const void *data, size_t len)
{
   return sketch_fnv1a(data, len);
}

void hashline_display_tag(uint64_t digest, char *buf, size_t buf_len)
{
   static const char hexd[] = "0123456789abcdef";
   if (!buf || buf_len == 0)
      return;
   size_t n = HASHLINE_DISPLAY_TAG_HEX;
   if (n > buf_len - 1)
      n = buf_len - 1;
   /* Take the low nibbles of the digest, most-significant of the chosen window
    * first, so the tag is a stable function of the digest. */
   for (size_t i = 0; i < n; i++)
      buf[i] = hexd[(digest >> (4 * (n - 1 - i))) & 0xF];
   buf[n] = '\0';
}

/* Iterate newline-delimited lines. Each call returns the [start,end) byte range
 * of the next line (excluding the '\n'); *cursor advances past the '\n'. Returns
 * 1 while a line remains, 0 at end. A trailing no-newline segment is a line. */
static int next_line(const char *content, size_t len, size_t *cursor, size_t *start, size_t *end)
{
   if (*cursor >= len)
      return 0;
   size_t s = *cursor;
   size_t e = s;
   while (e < len && content[e] != '\n')
      e++;
   *start = s;
   *end = e;                        /* excludes '\n' */
   *cursor = (e < len) ? e + 1 : e; /* step past '\n' if present */
   return 1;
}

size_t hashline_line_count(const char *content, size_t len)
{
   size_t cursor = 0, s = 0, e = 0, n = 0;
   while (next_line(content, len, &cursor, &s, &e))
      n++;
   return n;
}

size_t hashline_line_digests(const char *content, size_t len, uint64_t *out, size_t max)
{
   size_t cursor = 0, s = 0, e = 0, n = 0;
   int first = 1;
   while (next_line(content, len, &cursor, &s, &e))
   {
      int had_term = (e < len); /* a '\n' terminator sits at content[e] */
      if (n < max && out)
         out[n] = hashline_digest64(content + s, e - s, first, had_term);
      first = 0;
      n++;
   }
   return n;
}

/* ---- immutable snapshot store ---- */

#define HL_SNAP_CAP            256              /* LRU-bounded live snapshots */
#define HL_SNAP_DEFAULT_TTL_MS (30 * 60 * 1000) /* 30 minutes */

typedef struct
{
   char *id;  /* opaque unique id; NULL => empty slot */
   char *sid; /* owning session (may be NULL) */
   char *path;
   uint64_t file_digest;
   size_t size;
   size_t line_count;
   uint64_t *line_digests;
   int64_t read_time_ms; /* mint time (monotonic ms) */
   int64_t last_used_ms; /* for LRU */
} hl_snap_t;

static hl_snap_t g_snaps[HL_SNAP_CAP];
static pthread_mutex_t g_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_ttl_ms = HL_SNAP_DEFAULT_TTL_MS;
static uint64_t g_snap_seq; /* monotonic; guarantees id uniqueness (under g_snap_lock) */

static void hl_slot_free(hl_snap_t *s)
{
   free(s->id);
   free(s->sid);
   free(s->path);
   free(s->line_digests);
   memset(s, 0, sizeof(*s));
}

static int hl_expired(const hl_snap_t *s, int64_t now)
{
   return g_ttl_ms > 0 && (now - s->read_time_ms) > g_ttl_ms;
}

/* Caller holds g_snap_lock. Find a free slot, evicting the LRU (or an expired
 * one) if the store is full. Never returns NULL. */
static hl_snap_t *hl_acquire_slot(int64_t now)
{
   hl_snap_t *lru = NULL;
   for (int i = 0; i < HL_SNAP_CAP; i++)
   {
      hl_snap_t *s = &g_snaps[i];
      if (!s->id)
         return s;
      if (hl_expired(s, now))
      {
         hl_slot_free(s);
         return s;
      }
      if (!lru || s->last_used_ms < lru->last_used_ms)
         lru = s;
   }
   hl_slot_free(lru);
   return lru;
}

char *hashline_snapshot_mint(const char *sid, const char *path, const char *content, size_t len)
{
   char rnd[33];
   if (platform_random_hex(rnd, 32) != 0)
      return NULL;

   /* Allocate EVERYTHING (incl. the caller-owned return id) before publishing to
    * the store, so an OOM cannot leave an orphan snapshot under an id the caller
    * never received. On any failure the store is left unchanged. */
   size_t nlines = hashline_line_count(content, len);
   uint64_t *digs = NULL;
   if (nlines > 0)
   {
      digs = malloc(nlines * sizeof(uint64_t));
      if (!digs)
         return NULL;
      hashline_line_digests(content, len, digs, nlines);
   }
   char *path_copy = safe_strdup(path ? path : "");
   char *sid_copy = sid ? safe_strdup(sid) : NULL;
   if (!path_copy || (sid && !sid_copy))
   {
      free(digs);
      free(path_copy);
      free(sid_copy);
      return NULL;
   }

   int64_t now = util_now_ms();
   pthread_mutex_lock(&g_snap_lock);
   /* Monotonic sequence guarantees a unique id with no collision scan; the
    * 128-bit random suffix keeps the token opaque and unguessable. */
   uint64_t seq = ++g_snap_seq;
   char idbuf[64];
   snprintf(idbuf, sizeof(idbuf), "s%016llx%s", (unsigned long long)seq, rnd);
   char *id = safe_strdup(idbuf);  /* store's owned copy */
   char *ret = safe_strdup(idbuf); /* caller's owned copy */
   if (!id || !ret)
   {
      pthread_mutex_unlock(&g_snap_lock);
      free(id);
      free(ret);
      free(digs);
      free(path_copy);
      free(sid_copy);
      return NULL; /* store unchanged */
   }
   hl_snap_t *s = hl_acquire_slot(now);
   s->id = id;
   s->sid = sid_copy;
   s->path = path_copy;
   s->file_digest = hashline_digest64_raw(content, len);
   s->size = len;
   s->line_count = nlines;
   s->line_digests = digs;
   s->read_time_ms = now;
   s->last_used_ms = now;
   pthread_mutex_unlock(&g_snap_lock);
   return ret;
}

static int hl_sid_match(const char *a, const char *b)
{
   if (!a)
      return 1; /* caller did not scope the lookup */
   if (!b)
      return 0;
   return strcmp(a, b) == 0;
}

int hashline_snapshot_get(const char *sid, const char *snapshot_id, hashline_snapshot_view_t *out)
{
   if (!snapshot_id || !out)
      return 0;
   int64_t now = util_now_ms();
   pthread_mutex_lock(&g_snap_lock);
   for (int i = 0; i < HL_SNAP_CAP; i++)
   {
      hl_snap_t *s = &g_snaps[i];
      if (!s->id || strcmp(s->id, snapshot_id) != 0)
         continue;
      if (hl_expired(s, now))
      {
         hl_slot_free(s);
         break;
      }
      if (!hl_sid_match(sid, s->sid))
         break;
      /* Deep-copy into caller-owned buffers so the result stays valid even if the
       * slot is evicted (TTL/LRU/evict_all) right after we drop the lock. */
      uint64_t *digs = NULL;
      if (s->line_count > 0)
      {
         digs = malloc(s->line_count * sizeof(uint64_t));
         if (!digs)
            break; /* OOM => report a miss (caller re-reads) */
         memcpy(digs, s->line_digests, s->line_count * sizeof(uint64_t));
      }
      char *path_copy = safe_strdup(s->path ? s->path : "");
      if (!path_copy)
      {
         free(digs);
         break;
      }
      s->last_used_ms = now;
      out->path = path_copy;
      out->file_digest = s->file_digest;
      out->size = s->size;
      out->line_count = s->line_count;
      out->line_digests = digs;
      out->read_time_ms = s->read_time_ms;
      pthread_mutex_unlock(&g_snap_lock);
      return 1;
   }
   pthread_mutex_unlock(&g_snap_lock);
   return 0;
}

void hashline_snapshot_view_free(hashline_snapshot_view_t *out)
{
   if (!out)
      return;
   free(out->path);
   free(out->line_digests);
   memset(out, 0, sizeof(*out));
}

void hashline_snapshot_evict_all(void)
{
   pthread_mutex_lock(&g_snap_lock);
   for (int i = 0; i < HL_SNAP_CAP; i++)
      if (g_snaps[i].id)
         hl_slot_free(&g_snaps[i]);
   pthread_mutex_unlock(&g_snap_lock);
}

void hashline_snapshot_set_ttl_ms(int64_t ttl_ms)
{
   pthread_mutex_lock(&g_snap_lock);
   g_ttl_ms = (ttl_ms > 0) ? ttl_ms : HL_SNAP_DEFAULT_TTL_MS;
   pthread_mutex_unlock(&g_snap_lock);
}
