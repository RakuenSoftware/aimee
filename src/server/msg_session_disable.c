/* msg_session_disable.c: see msg_session_disable.h. Bounded open-addressing hash
 * set of disabled session keys with per-entry TTL. One global mutex guards the
 * table; lookups probe, expiry is lazy on hit, and a full-rebuild sweep (rate-
 * limited to 60s, or forced when the table crosses cap/2 on insert) reclaims
 * expired entries and tombstones. Eviction at capacity is expired-first then
 * oldest-inserted, so a live disable is never silently re-enabled while any expired
 * entry remains to reclaim. */
#include "msg_session_disable.h"

#include <ctype.h>
#include <pthread.h>
#include <string.h>

#include "gw_mutate_stats.h"
#include "harness_memory_common.h" /* hmem_sha256_hex — vendored SHA-256, no OpenSSL */
#include "util.h"                  /* util_now_ms (monotonic) */

#define MSG_SESSION_SLOTS             16384 /* power of two > cap, load factor ~0.61 at cap */
#define MSG_SESSION_CAP               10000
#define MSG_SESSION_SWEEP_INTERVAL_MS 60000

enum
{
   SLOT_EMPTY = 0,
   SLOT_USED = 1,
   SLOT_TOMB = 2
};

typedef struct
{
   char key[MSG_SESSION_KEY_LEN];
   int state;
   long long expires_ms;
   const char *reason; /* static literal, stored by pointer */
   unsigned long long seq;
} slot_t;

static slot_t g_tbl[MSG_SESSION_SLOTS];
static int g_used;
static unsigned long long g_seq;
static long long g_last_sweep_ms;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned long hash_key(const char *key)
{
   /* FNV-1a over the (<=16 char) key. */
   unsigned long h = 2166136261UL;
   for (const unsigned char *p = (const unsigned char *)key; *p; p++)
   {
      h ^= *p;
      h *= 16777619UL;
   }
   return h & (MSG_SESSION_SLOTS - 1);
}

/* Locate the slot holding `key`, or -1. Caller holds g_lock. */
static int find_slot_locked(const char *key)
{
   unsigned long i = hash_key(key);
   for (int probe = 0; probe < MSG_SESSION_SLOTS; probe++)
   {
      slot_t *s = &g_tbl[i];
      if (s->state == SLOT_EMPTY)
         return -1;
      if (s->state == SLOT_USED && strncmp(s->key, key, MSG_SESSION_KEY_LEN) == 0)
         return (int)i;
      i = (i + 1) & (MSG_SESSION_SLOTS - 1);
   }
   return -1;
}

/* Full rebuild: drop expired + tombstones, rehash live entries into a fresh table.
 * Caller holds g_lock. now_ms is the current monotonic clock. */
static void sweep_now_locked(long long now_ms)
{
   static slot_t rebuilt[MSG_SESSION_SLOTS];
   memset(rebuilt, 0, sizeof(rebuilt));
   int used = 0;
   for (int k = 0; k < MSG_SESSION_SLOTS; k++)
   {
      slot_t *s = &g_tbl[k];
      if (s->state != SLOT_USED || s->expires_ms <= now_ms)
         continue;
      unsigned long i = hash_key(s->key);
      while (rebuilt[i].state == SLOT_USED)
         i = (i + 1) & (MSG_SESSION_SLOTS - 1);
      rebuilt[i] = *s;
      used++;
   }
   memcpy(g_tbl, rebuilt, sizeof(g_tbl));
   g_used = used;
   g_last_sweep_ms = now_ms;
}

/* Evict the oldest-inserted live entry (all remaining are live post-sweep). Caller
 * holds g_lock. */
static void evict_oldest_locked(void)
{
   int oldest = -1;
   unsigned long long best = 0;
   for (int k = 0; k < MSG_SESSION_SLOTS; k++)
   {
      if (g_tbl[k].state != SLOT_USED)
         continue;
      if (oldest < 0 || g_tbl[k].seq < best)
      {
         best = g_tbl[k].seq;
         oldest = k;
      }
   }
   if (oldest >= 0)
   {
      g_tbl[oldest].state = SLOT_TOMB;
      g_used--;
   }
}

int msg_session_is_disabled(const char *key)
{
   if (!key || !key[0])
      return 0;
   long long now = util_now_ms();
   int disabled = 0;
   pthread_mutex_lock(&g_lock);
   int idx = find_slot_locked(key);
   if (idx >= 0)
   {
      if (g_tbl[idx].expires_ms > now)
         disabled = 1;
      else
      {
         g_tbl[idx].state = SLOT_TOMB; /* lazy expiry */
         g_used--;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return disabled;
}

void msg_session_disable(const char *key, int ttl_ms, const char *reason)
{
   if (!key || !key[0] || ttl_ms <= 0)
      return;
   long long now = util_now_ms();
   pthread_mutex_lock(&g_lock);

   int idx = find_slot_locked(key);
   if (idx >= 0)
   {
      /* Re-disable an existing session: refresh window + reason. */
      g_tbl[idx].expires_ms = now + ttl_ms;
      g_tbl[idx].reason = reason;
      pthread_mutex_unlock(&g_lock);
      gw_stat_inc_reason("session_disabled_set", reason ? reason : "unknown");
      return;
   }

   /* Reclaim before inserting when more than half full (proposal §2.4 cadence), and
    * always sweep immediately before an eviction so expired entries are dropped
    * first and a LIVE disable is never evicted while an expired one remains. The
    * >cap/2 rebuild is O(slots) per insert, but that only bites once >5000 sessions
    * are concurrently disabled — a catastrophic incident the breaker exists for, and
    * bounded because the table can never exceed cap. */
   if (g_used > MSG_SESSION_CAP / 2)
      sweep_now_locked(now);
   if (g_used >= MSG_SESSION_CAP)
      evict_oldest_locked();

   /* Insert at the first tombstone or empty slot on the probe chain. */
   unsigned long i = hash_key(key);
   int target = -1;
   for (int probe = 0; probe < MSG_SESSION_SLOTS; probe++)
   {
      if (g_tbl[i].state == SLOT_EMPTY)
      {
         if (target < 0)
            target = (int)i;
         break;
      }
      if (g_tbl[i].state == SLOT_TOMB && target < 0)
         target = (int)i;
      i = (i + 1) & (MSG_SESSION_SLOTS - 1);
   }
   if (target >= 0)
   {
      slot_t *s = &g_tbl[target];
      snprintf(s->key, sizeof(s->key), "%s", key);
      s->state = SLOT_USED;
      s->expires_ms = now + ttl_ms;
      s->reason = reason;
      s->seq = ++g_seq;
      g_used++;
   }
   pthread_mutex_unlock(&g_lock);
   gw_stat_inc_reason("session_disabled_set", reason ? reason : "unknown");
}

void msg_session_sweep(void)
{
   long long now = util_now_ms();
   pthread_mutex_lock(&g_lock);
   if (now - g_last_sweep_ms >= MSG_SESSION_SWEEP_INTERVAL_MS)
      sweep_now_locked(now);
   pthread_mutex_unlock(&g_lock);
}

size_t msg_session_count(void)
{
   long long now = util_now_ms();
   size_t n = 0;
   pthread_mutex_lock(&g_lock);
   for (int k = 0; k < MSG_SESSION_SLOTS; k++)
      if (g_tbl[k].state == SLOT_USED && g_tbl[k].expires_ms > now)
         n++;
   pthread_mutex_unlock(&g_lock);
   return n;
}

void msg_session_reset(void)
{
   pthread_mutex_lock(&g_lock);
   memset(g_tbl, 0, sizeof(g_tbl));
   g_used = 0;
   g_seq = 0;
   g_last_sweep_ms = 0;
   pthread_mutex_unlock(&g_lock);
}

/* --- session-key resolution ------------------------------------------------ */

static int is_16_lower_hex(const char *s)
{
   if (!s)
      return 0;
   for (int i = 0; i < 16; i++)
   {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return 0;
   }
   return s[16] == '\0'; /* exactly 16 chars */
}

msg_session_key_status_t msg_session_key_resolve(const char *hdr_session_id, const char *bearer,
                                                 const char *auth_identity,
                                                 char key[MSG_SESSION_KEY_LEN])
{
   char full[HMEM_HASH_HEX_LEN];
   int header_present_but_bad = 0;

   /* NULL-first: only validate a header when we have an identity to validate it
    * against (never SHA256(NULL)). */
   if (auth_identity && auth_identity[0] && hdr_session_id && hdr_session_id[0])
   {
      if (is_16_lower_hex(hdr_session_id))
      {
         hmem_sha256_hex(auth_identity, strlen(auth_identity), full);
         if (strncmp(hdr_session_id, full, 16) == 0)
         {
            memcpy(key, hdr_session_id, 16);
            key[16] = '\0';
            return MSG_SESSION_KEY_RESOLVED;
         }
      }
      /* Present but malformed or mismatched -> treat as absent, note the anomaly so
       * the caller can WARN (IP-rate-limited). An attacker's forged header for
       * another identity lands here and falls back to the attacker's own bearer. */
      header_present_but_bad = 1;
   }

   if (bearer && bearer[0])
   {
      hmem_sha256_hex(bearer, strlen(bearer), full);
      memcpy(key, full, 16);
      key[16] = '\0';
      return header_present_but_bad ? MSG_SESSION_KEY_BEARER_BAD_HDR : MSG_SESSION_KEY_RESOLVED;
   }

   /* Identity-less: no key, caller must pass through pristine with no disable state. */
   key[0] = '\0';
   return MSG_SESSION_KEY_NONE;
}
