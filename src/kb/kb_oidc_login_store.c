/* kb_oidc_login_store.c — see kb_oidc_login_store.h. */

#include "kb_oidc_login_store.h"

#include <openssl/crypto.h> /* CRYPTO_memcmp, OPENSSL_cleanse */
#include <pthread.h>
#include <string.h>

typedef struct
{
   kb_oidc_login_pending_t pending;
   int64_t expires_at;
   int occupied;
} slot_t;

/* Request threads serve /login/start and /login/callback concurrently, so the
 * table carries its own lock rather than relying on a caller convention. */
static slot_t g_slots[KB_OIDC_LOGIN_STORE_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Zeroes the secrets, not just the occupied flag: a freed slot must not leave a
 * code_verifier lying in memory for the lifetime of the process. */
static void slot_clear(slot_t *s)
{
   OPENSSL_cleanse(s, sizeof(*s));
}

static int sweep_locked(int64_t now)
{
   int reclaimed = 0;
   for (int i = 0; i < KB_OIDC_LOGIN_STORE_SLOTS; ++i)
      if (g_slots[i].occupied && g_slots[i].expires_at <= now)
      {
         slot_clear(&g_slots[i]);
         reclaimed++;
      }
   return reclaimed;
}

static int pending_valid(const kb_oidc_login_pending_t *p)
{
   /* A pending login that was never started has no secrets to retain, and
    * storing one would create a slot whose state matches the empty string. */
   return p && strnlen(p->state, sizeof(p->state)) == KB_OIDC_LOGIN_SECRET_LEN &&
          strnlen(p->code_verifier, sizeof(p->code_verifier)) == KB_OIDC_LOGIN_SECRET_LEN &&
          strnlen(p->nonce, sizeof(p->nonce)) == KB_OIDC_LOGIN_SECRET_LEN;
}

kb_oidc_login_store_result_t kb_oidc_login_store_put(const kb_oidc_login_pending_t *pending,
                                                     int64_t now, int ttl_seconds)
{
   if (!pending_valid(pending) || ttl_seconds > KB_OIDC_LOGIN_STORE_TTL_MAX)
      return KB_OIDC_LOGIN_STORE_INVALID;
   if (ttl_seconds <= 0)
      ttl_seconds = KB_OIDC_LOGIN_STORE_TTL_DEFAULT;

   pthread_mutex_lock(&g_lock);
   sweep_locked(now);
   kb_oidc_login_store_result_t rc = KB_OIDC_LOGIN_STORE_FULL;
   for (int i = 0; i < KB_OIDC_LOGIN_STORE_SLOTS; ++i)
   {
      if (g_slots[i].occupied)
      {
         /* Refuse a duplicate state rather than shadowing the earlier login.
          * With 256-bit states this cannot happen by chance, so it means the
          * caller reused a pending record — and the earlier login must stay
          * single-use. */
         if (CRYPTO_memcmp(g_slots[i].pending.state, pending->state, KB_OIDC_LOGIN_SECRET_LEN) == 0)
         {
            rc = KB_OIDC_LOGIN_STORE_INVALID;
            break;
         }
         continue;
      }
      g_slots[i].pending = *pending;
      g_slots[i].expires_at = now + ttl_seconds;
      g_slots[i].occupied = 1;
      rc = KB_OIDC_LOGIN_STORE_OK;
      break;
   }
   pthread_mutex_unlock(&g_lock);
   return rc;
}

kb_oidc_login_store_result_t kb_oidc_login_store_take(const char *state, int64_t now,
                                                      kb_oidc_login_pending_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!state || !out)
      return KB_OIDC_LOGIN_STORE_INVALID;
   /* A wrong-length state cannot match any slot, but it is answered as
    * _NOT_FOUND like every other miss: the callback handler must not be able to
    * tell "malformed" from "unknown". */
   if (strnlen(state, KB_OIDC_LOGIN_SECRET_LEN + 1) != KB_OIDC_LOGIN_SECRET_LEN)
      return KB_OIDC_LOGIN_STORE_NOT_FOUND;

   pthread_mutex_lock(&g_lock);
   /* Scan every slot unconditionally and record the hit, rather than returning
    * early. How long a lookup takes then reveals neither which slot matched nor
    * how many logins are pending. */
   int found = -1;
   for (int i = 0; i < KB_OIDC_LOGIN_STORE_SLOTS; ++i)
   {
      int live = g_slots[i].occupied && g_slots[i].expires_at > now;
      int match = CRYPTO_memcmp(g_slots[i].pending.state, state, KB_OIDC_LOGIN_SECRET_LEN) == 0;
      if (live && match && found < 0)
         found = i;
   }
   kb_oidc_login_store_result_t rc = KB_OIDC_LOGIN_STORE_NOT_FOUND;
   if (found >= 0)
   {
      *out = g_slots[found].pending;
      rc = KB_OIDC_LOGIN_STORE_OK;
   }
   /* Reclaim the consumed slot and anything else that has expired, so a login is
    * usable exactly once whether or not this one hit. */
   if (found >= 0)
      slot_clear(&g_slots[found]);
   sweep_locked(now);
   pthread_mutex_unlock(&g_lock);
   return rc;
}

int kb_oidc_login_store_sweep(int64_t now)
{
   pthread_mutex_lock(&g_lock);
   int reclaimed = sweep_locked(now);
   pthread_mutex_unlock(&g_lock);
   return reclaimed;
}

int kb_oidc_login_store_count(int64_t now)
{
   pthread_mutex_lock(&g_lock);
   int live = 0;
   for (int i = 0; i < KB_OIDC_LOGIN_STORE_SLOTS; ++i)
      if (g_slots[i].occupied && g_slots[i].expires_at > now)
         live++;
   pthread_mutex_unlock(&g_lock);
   return live;
}

void kb_oidc_login_store_reset(void)
{
   pthread_mutex_lock(&g_lock);
   OPENSSL_cleanse(g_slots, sizeof(g_slots));
   pthread_mutex_unlock(&g_lock);
}
