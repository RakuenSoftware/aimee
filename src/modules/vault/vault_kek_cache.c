/* vault_kek_cache.c: RAM-only, principal-keyed KEK cache (WP-C.1, D14). Holds the
 * raw 32-byte key-encryption key for each unlocked vault principal; reject-don't-
 * evict at capacity; OPENSSL_cleanse on every removal; TTL'd; mutex-guarded. See
 * vault_kek_cache.h. */
#include "vault_kek_cache.h"
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX */
#include <openssl/crypto.h>  /* OPENSSL_cleanse */
#include <pthread.h>
#include <signal.h>
#include <string.h>

typedef struct
{
   int used;
   char principal[VAULT_PRINCIPAL_MAX];
   uint8_t kek[VAULT_KEK_LEN];
   long expires_epoch; /* now_epoch + TTL at put; entry is dead once now >= this */
} kek_slot_t;

static kek_slot_t g_slots[VAULT_KEK_CACHE_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_fork_child_invalid;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static int g_atfork_status = -1;

void vault_kek_cache_after_fork_child(void)
{
   g_fork_child_invalid = 1;
   volatile unsigned char *p = (volatile unsigned char *)g_slots;
   for (size_t i = 0; i < sizeof(g_slots); i++)
      p[i] = 0;
}

static void register_atfork_once(void)
{
   g_atfork_status = pthread_atfork(NULL, NULL, vault_kek_cache_after_fork_child);
}

static int cache_ready(void)
{
   return !g_fork_child_invalid && pthread_once(&g_atfork_once, register_atfork_once) == 0 &&
                  g_atfork_status == 0
              ? 0
              : -1;
}

/* Cleanse the KEK + key fields and free the slot. Caller holds g_lock. */
static void slot_clear(kek_slot_t *s)
{
   OPENSSL_cleanse(s->kek, sizeof(s->kek));
   memset(s->principal, 0, sizeof(s->principal));
   s->expires_epoch = 0;
   s->used = 0;
}

/* Find a live slot for `principal`, cleansing+evicting it if expired. Returns the
 * slot or NULL. Caller holds g_lock. */
static kek_slot_t *slot_find_live(const char *principal, long now_epoch)
{
   for (int i = 0; i < VAULT_KEK_CACHE_SLOTS; i++)
   {
      kek_slot_t *s = &g_slots[i];
      if (!s->used || strcmp(s->principal, principal) != 0)
         continue;
      if (now_epoch >= s->expires_epoch)
      {
         slot_clear(s); /* expired -> wipe, never return a stale key */
         return NULL;
      }
      return s;
   }
   return NULL;
}

int vault_kek_cache_put(const char *principal, const uint8_t kek[VAULT_KEK_LEN], long now_epoch)
{
   if (cache_ready() != 0 || !principal || !principal[0] || !kek ||
       strlen(principal) >= VAULT_PRINCIPAL_MAX)
      return -1;

   pthread_mutex_lock(&g_lock);
   int rc = -1;

   /* Refresh an existing entry (also reaping it first if it had expired). */
   kek_slot_t *s = slot_find_live(principal, now_epoch);
   if (!s)
   {
      /* Reuse a free OR already-expired slot. Reap expired entries as we scan so
       * a full-of-stale cache still accepts a new principal. */
      for (int i = 0; i < VAULT_KEK_CACHE_SLOTS; i++)
      {
         if (g_slots[i].used && now_epoch >= g_slots[i].expires_epoch)
            slot_clear(&g_slots[i]);
         if (!g_slots[i].used && !s)
            s = &g_slots[i];
      }
   }
   if (s)
   {
      s->used = 1;
      snprintf(s->principal, sizeof(s->principal), "%s", principal);
      memcpy(s->kek, kek, VAULT_KEK_LEN);
      s->expires_epoch = now_epoch + VAULT_KEK_CACHE_TTL_SECONDS;
      rc = 0;
   }
   /* else: cache full of LIVE principals -> reject (never evict an in-use KEK). */

   pthread_mutex_unlock(&g_lock);
   return rc;
}

int vault_kek_cache_get(const char *principal, long now_epoch, uint8_t kek_out[VAULT_KEK_LEN])
{
   if (cache_ready() != 0 || !principal || !kek_out)
   {
      if (kek_out)
         OPENSSL_cleanse(kek_out, VAULT_KEK_LEN);
      return -1;
   }
   pthread_mutex_lock(&g_lock);
   kek_slot_t *s = slot_find_live(principal, now_epoch);
   int rc = -1;
   if (s)
   {
      memcpy(kek_out, s->kek, VAULT_KEK_LEN);
      rc = 0;
   }
   pthread_mutex_unlock(&g_lock);
   if (rc != 0)
      OPENSSL_cleanse(kek_out, VAULT_KEK_LEN);
   return rc;
}

int vault_kek_cache_has(const char *principal, long now_epoch)
{
   if (cache_ready() != 0 || !principal)
      return 0;
   pthread_mutex_lock(&g_lock);
   int has = slot_find_live(principal, now_epoch) != NULL;
   pthread_mutex_unlock(&g_lock);
   return has;
}

void vault_kek_cache_evict(const char *principal)
{
   if (cache_ready() != 0 || !principal)
      return;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < VAULT_KEK_CACHE_SLOTS; i++)
      if (g_slots[i].used && strcmp(g_slots[i].principal, principal) == 0)
         slot_clear(&g_slots[i]);
   pthread_mutex_unlock(&g_lock);
}

void vault_kek_cache_clear(void)
{
   if (cache_ready() != 0)
      return;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < VAULT_KEK_CACHE_SLOTS; i++)
      if (g_slots[i].used)
         slot_clear(&g_slots[i]);
   pthread_mutex_unlock(&g_lock);
}

int vault_kek_cache_count(long now_epoch)
{
   if (cache_ready() != 0)
      return 0;
   pthread_mutex_lock(&g_lock);
   int n = 0;
   for (int i = 0; i < VAULT_KEK_CACHE_SLOTS; i++)
   {
      if (!g_slots[i].used)
         continue;
      if (now_epoch >= g_slots[i].expires_epoch)
         slot_clear(&g_slots[i]);
      else
         n++;
   }
   pthread_mutex_unlock(&g_lock);
   return n;
}
