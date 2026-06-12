/* vault_service.c: credential-vault business logic + gating (WP-C.1). Bridges the
 * thin route handlers / delegate use-path to the crypto/cache/store primitives.
 * Free of cJSON + connection I/O so the security gates are unit-testable. See
 * vault_service.h. */
#include "vault_service.h"
#include "vault_crypto.h"
#include "vault_kek_cache.h"
#include "vault_store.h"
#include "log.h"
#include <openssl/crypto.h>
#include <string.h>

const char *vault_status_str(vault_status_t s)
{
   switch (s)
   {
   case VAULT_OK:
      return "ok";
   case VAULT_NO_ENTRY:
      return "no_entry";
   case VAULT_ERR_UNATTESTED:
      return "unattested";
   case VAULT_ERR_TRANSPORT:
      return "transport_not_allowed";
   case VAULT_ERR_LOCKED:
      return "locked";
   case VAULT_ERR_BADARG:
      return "bad_argument";
   case VAULT_ERR_CRYPTO:
      return "crypto_error";
   case VAULT_ERR_IO:
      return "io_error";
   }
   return "error";
}

vault_status_t vault_service_unlock(const char *principal, attested_transport_t transport,
                                    const uint8_t *root_key, size_t root_key_len, long now_epoch)
{
   /* Fail-closed identity gate: a blank principal is an un-attested conn (or a
    * missed WP-C.0 hop) — never act as uid:0. */
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   /* The CLI root-key push is UDS-only (a kernel-attested local peer). Refuse it
    * over TCP and refuse a webuser principal here (its KEK comes from scrypt at
    * login, WP-C.2). */
   if (transport != ATTEST_UDS_PEERCRED)
      return VAULT_ERR_TRANSPORT;
   if (!root_key || root_key_len != VAULT_ROOT_KEY_LEN)
      return VAULT_ERR_BADARG;

   uint8_t salt[VAULT_SALT_LEN];
   if (vault_store_get_or_create_salt(principal, salt) != 0)
      return VAULT_ERR_IO;

   uint8_t kek[VAULT_KEK_LEN];
   vault_status_t st = VAULT_OK;
   if (vault_kek_derive(root_key, root_key_len, salt, sizeof(salt), kek) != 0)
      st = VAULT_ERR_CRYPTO;
   else if (vault_kek_cache_put(principal, kek, now_epoch) != 0)
      st = VAULT_ERR_LOCKED; /* cache full of live principals — reject, don't evict */

   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(salt, sizeof(salt));
   if (st == VAULT_OK)
      LOG_INFO("vault", "unlocked principal (cred count managed per-op)");
   return st;
}

vault_status_t vault_service_set(const char *principal, const char *agent, const char *cred,
                                 const char *secret, long now_epoch)
{
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   if (!agent || !agent[0] || !cred || !cred[0] || !secret)
      return VAULT_ERR_BADARG;

   uint8_t kek[VAULT_KEK_LEN];
   if (vault_kek_cache_get(principal, now_epoch, kek) != 0)
      return VAULT_ERR_LOCKED; /* must unlock first */

   vault_status_t st = vault_store_set(principal, kek, agent, cred, secret) == 0 ? VAULT_OK
                                                                                 : VAULT_ERR_IO;
   OPENSSL_cleanse(kek, sizeof(kek));
   return st;
}

vault_status_t vault_service_get(const char *principal, const char *agent, const char *cred,
                                 char *out, size_t out_len, long now_epoch)
{
   if (out && out_len)
      out[0] = '\0';
   /* No attested principal => no vault for this caller; fall back silently. */
   if (!principal || !principal[0])
      return VAULT_NO_ENTRY;
   if (!agent || !cred || !out || !out_len)
      return VAULT_ERR_BADARG;

   /* Distinguish "no such credential" (fall back to env, D15) from "exists but
    * locked" (hard error). Check existence WITHOUT the KEK first. */
   if (!vault_store_has_entry(principal, agent, cred))
      return VAULT_NO_ENTRY;

   uint8_t kek[VAULT_KEK_LEN];
   if (vault_kek_cache_get(principal, now_epoch, kek) != 0)
   {
      /* The credential is vaulted but the KEK is gone (locked / TTL expired):
       * fail closed — never silently downgrade to an env credential. */
      LOG_WARN("vault", "credential present but vault locked for agent=%s cred=%s", agent, cred);
      return VAULT_ERR_LOCKED;
   }

   int rc = vault_store_get(principal, kek, agent, cred, out, out_len);
   OPENSSL_cleanse(kek, sizeof(kek));
   if (rc == 0)
      return VAULT_OK;
   if (rc == VAULT_STORE_NO_ENTRY)
      return VAULT_NO_ENTRY; /* raced with a delete */
   return VAULT_ERR_CRYPTO;  /* decrypt/tamper — fail closed */
}

vault_status_t vault_service_list(const char *principal, vault_store_entry_t *out, int max,
                                  int *count)
{
   if (count)
      *count = 0;
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   int n = vault_store_list(principal, out, max);
   if (n < 0)
      return VAULT_ERR_IO;
   if (count)
      *count = n;
   return VAULT_OK;
}

vault_status_t vault_service_delete(const char *principal, const char *agent, const char *cred)
{
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   if (!agent || !agent[0] || !cred || !cred[0])
      return VAULT_ERR_BADARG;
   return vault_store_delete(principal, agent, cred) == 0 ? VAULT_OK : VAULT_ERR_IO;
}

vault_status_t vault_service_lock(const char *principal)
{
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   vault_kek_cache_evict(principal);
   return VAULT_OK;
}
