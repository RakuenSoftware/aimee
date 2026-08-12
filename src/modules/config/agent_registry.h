/* agent_registry.h: the config module's cached agent registry.
 *
 * Private to the config module plus its loader in src/server/agent_config.c.
 * Callers outside that pair want the accessors in agent_config.h
 * (agent_registry_find / agent_registry_default_primary), which never name
 * agent_config_t and therefore never copy 350,968 bytes. */
#ifndef DEC_AGENT_REGISTRY_H
#define DEC_AGENT_REGISTRY_H 1

#include <sys/stat.h>

#include "agent_types.h"

/* Copy the cached registry out when `st` still identifies the cached file.
 * Returns 0 on a hit (cfg filled), non-zero on a miss (cfg untouched). */
int agent_registry_cache_get(const struct stat *st, agent_config_t *cfg);

/* Publish a freshly parsed registry, keyed by the stat() of the file it came
 * from. Takes a copy; the caller keeps ownership of `cfg`. */
void agent_registry_cache_put(const agent_config_t *cfg, const struct stat *st);

/* Drop the cache: the next read reloads. */
void agent_registry_cache_invalidate(void);

/* Run `pick` against the cached registry WITHOUT copying it, and copy out only
 * the agent it selects. Returns 0 when an agent was selected and copied, 1 when
 * the cache was current but `pick` found nothing (so the caller must not retry
 * -- the answer is genuinely "no such agent"), and negative when the cache was
 * cold or stale (the caller should load and try again).
 *
 * Distinguishing 1 from negative is what keeps a lookup for a nonexistent agent
 * from forcing a full reload on every request. */
int agent_registry_pick_cached(const struct stat *st, agent_t *out,
                               agent_t *(*pick)(agent_config_t *, const void *), const void *arg);

#endif /* DEC_AGENT_REGISTRY_H */
