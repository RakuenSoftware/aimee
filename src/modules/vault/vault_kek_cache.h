#ifndef DEC_VAULT_KEK_CACHE_H
#define DEC_VAULT_KEK_CACHE_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h" /* VAULT_KEK_LEN */

/* vault_kek_cache: the RAM-only, principal-keyed key-encryption-key cache for the
 * credential vault (WP-C.1, D14). The KEK is derived once per unlock (CLI) or
 * login (webchat) and held here so per-delegate credential decrypts don't need
 * the client root key / password again.
 *
 * Why a purpose-built cache instead of the forge_credentials broker (D14): that
 * broker is a NUL-terminated *string* store (truncates a binary KEK at the first
 * 0x00) and has only 64 slots with evict-on-full. This cache holds the RAW
 * 32-byte KEK byte-exact, is keyed by the attested PRINCIPAL string (never a
 * client-supplied session_id), and is **reject-don't-evict** when full so a burst
 * of unlocks can never silently evict an in-use KEK. It copies the broker's
 * discipline: RAM only (never to disk, never logged), OPENSSL_cleanse on every
 * eviction/expiry/clear, TTL'd, and mutex-guarded.
 *
 * Time is passed in (now_epoch) so the TTL is testable without a real clock. */

#define VAULT_KEK_CACHE_SLOTS       64  /* concurrent principals; reject-don't-evict at capacity */
#define VAULT_KEK_CACHE_TTL_SECONDS 900 /* 15 min; after this the KEK is wiped + evicted */

/* Insert or refresh the KEK for `principal`. If the principal is already cached,
 * its KEK is overwritten and its TTL reset. If not and the cache is full, the
 * call is REJECTED (-1) rather than evicting an in-use KEK. The 32 bytes at `kek`
 * are copied byte-exact (embedded 0x00 preserved). 0 on success, -1 on
 * reject/bad-args. The caller should OPENSSL_cleanse its own copy of `kek`
 * afterward. */
int vault_kek_cache_put(const char *principal, const uint8_t kek[VAULT_KEK_LEN], long now_epoch);

/* Fetch the KEK for `principal` into `kek_out` (32 bytes). Returns 0 if present
 * and not expired; -1 otherwise. An entry past its TTL is OPENSSL_cleanse'd and
 * evicted on access (so a delegate after expiry hits the fail-closed "vault
 * locked" path, never a stale key). */
int vault_kek_cache_get(const char *principal, long now_epoch, uint8_t kek_out[VAULT_KEK_LEN]);

/* True (1) if `principal` has a live (unexpired) KEK cached, else 0. Evicts an
 * expired entry as a side effect, like _get. */
int vault_kek_cache_has(const char *principal, long now_epoch);

/* Cleanse + drop the principal's KEK (the `vault lock` path / logout). No-op if
 * absent. */
void vault_kek_cache_evict(const char *principal);

/* Cleanse + drop every cached KEK (server shutdown / test teardown). */
void vault_kek_cache_clear(void);

/* Number of live slots currently held (after evicting any expired) — for
 * observability + tests. */
int vault_kek_cache_count(long now_epoch);

#endif /* DEC_VAULT_KEK_CACHE_H */
