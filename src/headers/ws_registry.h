#ifndef WS_REGISTRY_H
#define WS_REGISTRY_H 1

#include <stddef.h>

/* ws_registry — the deployment-global project-key registry + lifecycle lock
 * (webchat project lifecycle proposal, Identity model).
 *
 * Registry: one entry per live project ref mapping key -> {credential-free
 * canonical remote, holder count}, under <webusers_base>/.registry/. Entry
 * files are named by the ref with '/' encoded as '%' (both components are
 * ws_scope_name_valid, so '%' cannot collide) and written atomically
 * (tmp + rename). Mutations happen ONLY under the ref's lifecycle lock.
 *
 * Lifecycle lock: flock(LOCK_EX) on <webusers_base>/.locks/<first component>.
 * Keying on the FIRST component (org of "acme/foo", bare name of flat "acme")
 * serializes flat-vs-org namespace conflicts, publication, deletion, and
 * org pruning across the whole deployment. */

/* Acquire the lifecycle lock for `ref`'s first component. Blocks until held.
 * Returns the lock fd (close() releases), or -1. */
int ws_reg_lock(const char *ref);

/* Look up `ref`. Returns 1 with *holders / remote[cap] filled when present,
 * 0 when absent, -1 on error. */
int ws_reg_lookup(const char *ref, char *remote, size_t remote_cap, int *holders);

/* Register a holder of `ref` with `remote` (credential-free canonical form).
 * Creates the entry at holders=1, or increments when the recorded remote
 * matches exactly. Returns 0 on success, 1 on remote mismatch (conflict —
 * caller 409s), -1 on error. Caller MUST hold the lifecycle lock. */
int ws_reg_register(const char *ref, const char *remote);

/* Remove one holder of `ref`; deletes the entry when the count hits 0.
 * Returns the remaining holder count (>= 0), or -1 on error/absent entry.
 * Caller MUST hold the lifecycle lock. (The 0 return IS the last-holder
 * signal for the delete path.) */
int ws_reg_unregister(const char *ref);

/* Re-derive one ref's registry entry from the git config of every published
 * clone at that ref (honors `git remote set-url`; refreshes sidecars; removes
 * the entry when no holder remains). Caller MUST hold the lifecycle lock.
 * Returns 0 or -1. */
int ws_reg_resync(const char *ref);

/* 1 iff the registry is ready (the startup rebuild succeeded). The first call
 * runs the rebuild lazily under a mutex; a FAILED rebuild keeps the surface
 * disabled (a partially-populated authoritative registry must never accept
 * registrations) and is retried on the next call. */
int ws_reg_ready(void);

/* Rebuild the registry from the published clones on disk (each webuser tree's
 * sidecars). Crash-window drift self-heals here; counts derive from PUBLISHED
 * clones only. Called once at server startup. Returns 0 or -1. */
int ws_reg_rebuild(void);

#endif /* WS_REGISTRY_H */
