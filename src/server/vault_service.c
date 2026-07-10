/* vault_service.c: credential-vault business logic + gating (WP-C.1). Bridges the
 * thin route handlers / delegate use-path to the crypto/cache/store primitives.
 * Free of cJSON + connection I/O so the security gates are unit-testable. See
 * vault_service.h. */
#include "vault_service.h"
#include "vault_crypto.h"
#include "vault_kek_cache.h"
#include "vault_server_key.h"
#include "vault_store.h"
#include "log.h"
#include <openssl/crypto.h>
#include <stdlib.h>
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

/* Add the server wrap to any user-only credentials for `principal` using the live
 * user KEK (WP-C.4 dual-access backfill). Best-effort; logs on failure. */
static void vault_service_backfill_server_wraps(const char *principal,
                                                const uint8_t user_kek[VAULT_KEK_LEN])
{
   uint8_t server_kek[VAULT_KEK_LEN];
   if (vault_server_kek(server_kek) != 0)
      return;
   if (vault_store_add_server_wraps(principal, user_kek, server_kek) != 0)
      LOG_WARN("vault", "server-wrap backfill failed (creds remain user-only until next unlock)");
   OPENSSL_cleanse(server_kek, sizeof(server_kek));
}

/* The autonomous server use-path: decrypt (agent, cred) via the server wrap, no
 * client unlock / KEK cache. Returns VAULT_OK (plaintext written), VAULT_NO_ENTRY
 * (no such cred, or a legacy user-only entry — caller falls back to the user KEK
 * path), or VAULT_ERR_* (fail closed). */
vault_status_t vault_service_get_server_wrap(const char *principal, const char *agent,
                                             const char *cred, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!principal || !principal[0])
      return VAULT_NO_ENTRY;
   if (!agent || !cred || !out || !out_len)
      return VAULT_ERR_BADARG;
   uint8_t server_kek[VAULT_KEK_LEN];
   if (vault_server_kek(server_kek) != 0)
      return VAULT_ERR_CRYPTO;
   int rc = vault_store_get_server(principal, server_kek, agent, cred, out, out_len);
   OPENSSL_cleanse(server_kek, sizeof(server_kek));
   if (rc == 0)
      return VAULT_OK;
   if (rc == VAULT_STORE_NO_ENTRY)
      return VAULT_NO_ENTRY;
   return VAULT_ERR_CRYPTO; /* decrypt/tamper — fail closed */
}

/* Read (agent, cred) from the SERVER principal's vault under the server master KEK
 * — no client, no unlock. VAULT_OK / VAULT_NO_ENTRY (no file or no entry) /
 * VAULT_ERR_* (fail closed). */
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!agent || !cred || !out || !out_len)
      return VAULT_ERR_BADARG;
   uint8_t kek[VAULT_KEK_LEN];
   if (vault_server_kek(kek) != 0)
      return VAULT_ERR_CRYPTO;
   int rc = vault_store_get(VAULT_SERVER_PRINCIPAL, kek, agent, cred, out, out_len);
   OPENSSL_cleanse(kek, sizeof(kek));
   if (rc == 0)
      return VAULT_OK;
   if (rc == VAULT_STORE_NO_ENTRY)
      return VAULT_NO_ENTRY;
   return VAULT_ERR_CRYPTO; /* decrypt/tamper — fail closed */
}

vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret)
{
   if (!agent || !agent[0] || !cred || !cred[0] || !secret)
      return VAULT_ERR_BADARG;
   uint8_t kek[VAULT_KEK_LEN];
   if (vault_server_kek(kek) != 0)
      return VAULT_ERR_CRYPTO; /* fail-closed: no server KEK -> never store plaintext */

   /* The vault file must exist before set; create it (the stored salt is unused
    * for the server KEK, which is derived from the master key, not salt+root). */
   uint8_t salt[VAULT_SALT_LEN];
   vault_status_t st;
   if (vault_store_get_or_create_salt(VAULT_SERVER_PRINCIPAL, salt) != 0)
      st = VAULT_ERR_IO;
   else
      st = vault_store_set(VAULT_SERVER_PRINCIPAL, kek, agent, cred, secret) == 0 ? VAULT_OK
                                                                                  : VAULT_ERR_IO;
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(salt, sizeof(salt));
   return st;
}

vault_status_t vault_service_set_server_wrap(const char *principal, const char *agent,
                                             const char *cred, const char *secret)
{
   if (!principal || !principal[0] || !agent || !agent[0] || !cred || !cred[0] || !secret)
      return VAULT_ERR_BADARG;
   uint8_t server_kek[VAULT_KEK_LEN];
   if (vault_server_kek(server_kek) != 0)
      return VAULT_ERR_CRYPTO; /* fail-closed: no server KEK -> never store plaintext */

   /* The principal's vault file must exist before set; create it (with its salt)
    * so a webuser who has never unlocked can still have a server-wrapped cred
    * sealed. The salt is unused for the server wrap (server KEK derives from the
    * master key), but a later unlock uses this same salt to derive the user KEK. */
   uint8_t salt[VAULT_SALT_LEN];
   vault_status_t st;
   if (vault_store_get_or_create_salt(principal, salt) != 0)
      st = VAULT_ERR_IO;
   else
      st = vault_store_set_server(principal, server_kek, agent, cred, secret) == 0 ? VAULT_OK
                                                                                   : VAULT_ERR_IO;
   OPENSSL_cleanse(server_kek, sizeof(server_kek));
   OPENSSL_cleanse(salt, sizeof(salt));
   return st;
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
   else if (vault_store_unlock_check(principal, kek) != 0)
      st = VAULT_ERR_CRYPTO; /* wrong root key — caught by the key-check verifier */
   else if (vault_kek_cache_put(principal, kek, now_epoch) != 0)
      st = VAULT_ERR_LOCKED; /* cache full of live principals — reject, don't evict */

   /* WP-C.4 dual-access backfill: now that we hold the user KEK, add the server
    * wrap to any credentials written before dual-wrap so the server can decrypt
    * them autonomously. Best-effort — never fail the unlock on a backfill error. */
   if (st == VAULT_OK)
      vault_service_backfill_server_wraps(principal, kek);

   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(salt, sizeof(salt));
   if (st == VAULT_OK)
      LOG_INFO("vault", "unlocked principal (cred count managed per-op)");
   return st;
}

vault_status_t vault_service_unlock_password(const char *principal, attested_transport_t transport,
                                             const uint8_t *password, size_t password_len,
                                             long now_epoch)
{
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   /* The password unlock is for the webchat-asserted webuser principal only,
    * honored under the server.token trust boundary (ATTEST_WEBCHAT_TRUSTED). A
    * uid:/TCP conn must use the root-key unlock instead. */
   if (transport != ATTEST_WEBCHAT_TRUSTED)
      return VAULT_ERR_TRANSPORT;
   if (!password || password_len == 0)
      return VAULT_ERR_BADARG;

   uint8_t salt[VAULT_SALT_LEN];
   if (vault_store_get_or_create_salt(principal, salt) != 0)
      return VAULT_ERR_IO;

   uint8_t kek[VAULT_KEK_LEN];
   vault_status_t st = VAULT_OK;
   if (vault_kek_derive_scrypt(password, password_len, salt, sizeof(salt), kek) != 0)
      st = VAULT_ERR_CRYPTO;
   else if (vault_store_unlock_check(principal, kek) != 0)
      st = VAULT_ERR_CRYPTO; /* wrong password — caught by the key-check verifier */
   else if (vault_kek_cache_put(principal, kek, now_epoch) != 0)
      st = VAULT_ERR_LOCKED;

   /* WP-C.4 dual-access backfill (see vault_service_unlock). */
   if (st == VAULT_OK)
      vault_service_backfill_server_wraps(principal, kek);

   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(salt, sizeof(salt));
   if (st == VAULT_OK)
      LOG_INFO("vault", "webuser vault unlocked (scrypt)");
   return st;
}

vault_status_t vault_service_rekey_password(const char *principal, attested_transport_t transport,
                                            const uint8_t *old_password, size_t old_len,
                                            const uint8_t *new_password, size_t new_len,
                                            long now_epoch)
{
   if (!principal || !principal[0])
      return VAULT_ERR_UNATTESTED;
   if (transport != ATTEST_WEBCHAT_TRUSTED)
      return VAULT_ERR_TRANSPORT;
   if (!old_password || old_len == 0 || !new_password || new_len == 0)
      return VAULT_ERR_BADARG;

   /* Read-only: a rekey against a non-existent principal must fail closed, never
    * materialize a vault an attacker could then own. */
   uint8_t salt[VAULT_SALT_LEN];
   if (vault_store_salt_readonly(principal, salt) != 0)
      return VAULT_ERR_CRYPTO;

   uint8_t old_kek[VAULT_KEK_LEN], new_kek[VAULT_KEK_LEN];
   vault_status_t st = VAULT_OK;
   if (vault_kek_derive_scrypt(old_password, old_len, salt, sizeof(salt), old_kek) != 0 ||
       vault_kek_derive_scrypt(new_password, new_len, salt, sizeof(salt), new_kek) != 0)
      st = VAULT_ERR_CRYPTO;
   else if (vault_store_rekey(principal, old_kek, new_kek) != 0)
      st = VAULT_ERR_CRYPTO; /* wrong old password / tamper -> fail closed, vault untouched */
   else if (vault_kek_cache_put(principal, new_kek, now_epoch) != 0)
      st = VAULT_ERR_LOCKED; /* re-wrap succeeded but cache full; caller can re-unlock */

   OPENSSL_cleanse(old_kek, sizeof(old_kek));
   OPENSSL_cleanse(new_kek, sizeof(new_kek));
   OPENSSL_cleanse(salt, sizeof(salt));
   if (st == VAULT_OK)
      LOG_INFO("vault", "webuser vault re-keyed (password change)");
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

   /* WP-C.4 dual-access: wrap the DEK under BOTH the user KEK and the server KEK
    * so the server can decrypt this credential autonomously after a restart.
    * Fail-closed if the server KEK is unavailable — never store a user-only cred
    * the server cannot later read (that is the bug this whole change fixes). */
   uint8_t server_kek[VAULT_KEK_LEN];
   vault_status_t st;
   if (vault_server_kek(server_kek) != 0)
      st = VAULT_ERR_CRYPTO;
   else
      st = vault_store_set_dual(principal, kek, server_kek, agent, cred, secret) == 0
               ? VAULT_OK
               : VAULT_ERR_IO;
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(server_kek, sizeof(server_kek));
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

vault_status_t vault_service_inject_api_key(const char *principal, const char *agent, char *api_key,
                                            size_t api_key_len, long now_epoch)
{
   if (!api_key || !api_key_len)
      return VAULT_ERR_BADARG;
   char *tmp = malloc(api_key_len);
   if (!tmp)
      return VAULT_ERR_IO;
   /* Server-first (WP-C.4): the server wrap decrypts autonomously — no client
    * unlock, survives restart. Fall back to the user-KEK path only for legacy
    * creds not yet dual-wrapped (those get backfilled at the next user unlock). */
   vault_status_t st =
       vault_service_get_server_wrap(principal, agent, VAULT_API_KEY_CRED, tmp, api_key_len);
   if (st == VAULT_NO_ENTRY)
      st = vault_service_get(principal, agent, VAULT_API_KEY_CRED, tmp, api_key_len, now_epoch);
   /* Last: the server-owned vault (delegate creds pushed from a TCP thin client,
    * which has no per-user principal). Only on a clean miss — a LOCKED user cred
    * stays a hard error (D15), never silently downgraded to the server vault. */
   if (st == VAULT_NO_ENTRY)
      st = vault_service_get_server_principal(agent, VAULT_API_KEY_CRED, tmp, api_key_len);
   if (st == VAULT_OK)
      snprintf(api_key, api_key_len, "%s", tmp); /* overwrite only on a real hit */
   OPENSSL_cleanse(tmp, api_key_len);
   free(tmp);
   return st;
}
