/* provider_catalog.h: live per-provider health, locality, and effective
 * concurrency catalog.
 *
 * The catalog is a process-global singleton.  Call
 * provider_catalog_init() once at startup with the agent list, then
 * record_success / record_failure from the delegate hot path. The
 * concurrency subsystem and doctor surfaces query it read-only.
 */
#ifndef DEC_PROVIDER_CATALOG_H
#define DEC_PROVIDER_CATALOG_H 1

#include "config.h"
#include "agent_types.h"
#include <time.h>

/* Endpoint locality — set from the endpoint URL at init time. */
typedef enum
{
   PROVIDER_LOCALITY_UNKNOWN = 0,
   PROVIDER_LOCALITY_LOCAL, /* loopback / unix socket / localhost */
   PROVIDER_LOCALITY_LAN,   /* RFC-1918 or link-local */
   PROVIDER_LOCALITY_REMOTE /* public internet or unclassified */
} provider_locality_t;

/* Coarse health state derived from recent outcome history. */
typedef enum
{
   CATALOG_HEALTH_HEALTHY = 0, /* last call succeeded, streak clear */
   CATALOG_HEALTH_DEGRADED,    /* 1–2 consecutive failures */
   CATALOG_HEALTH_STALE,       /* no activity for > stale_seconds */
   CATALOG_HEALTH_DOWN         /* 3+ consecutive failures */
} catalog_health_t;

/* Seconds since last success before an entry is considered stale. */
#define PROVIDER_STALE_SECONDS 3600
/* Seconds since last success before an entry is considered down due to age. */
#define PROVIDER_DOWN_SECONDS (3 * PROVIDER_STALE_SECONDS)
/* Cooldown after a streak-induced DOWN before the circuit breaker half-opens:
 * the agent becomes routable again so a probe can let it recover without a
 * server restart. Measured from the last failure. */
#define PROVIDER_DOWN_COOLDOWN_SECONDS 60

/* Locality-inferred default concurrency limits.
 * Only applied when no explicit per-model / per-provider config override
 * exists.  Config-explicit values always win. */
#define PROVIDER_LOCAL_CONCURRENCY_DEFAULT 1
#define PROVIDER_LAN_CONCURRENCY_DEFAULT   2

#define CATALOG_MAX_ENTRIES 64

/* Max concurrent provider slot holders tracked per agent (max_parallel is
 * clamped to this) and the TTL after which a slot with no matching release is
 * auto-reclaimed. The TTL is set well above the worst-case legitimate request
 * (provider timeout ~180s x up to 3 retries ~= 540s) so only truly-dead holders
 * are reaped, never a slow-but-live request. */
#define PROVIDER_SLOT_CAP     64
#define PROVIDER_SLOT_TTL_SEC 900

#include <time.h>
/* Compact holders older than `ttl` out of a per-agent acquire-timestamp array
 * kept sorted oldest-first; returns the new live count. Pure (no I/O, no locks)
 * so the reap arithmetic is unit-testable. */
int provider_slots_reap_stamps(time_t *stamps, int active, time_t now, int ttl);

typedef struct
{
   char agent_name[MAX_AGENT_NAME];
   char endpoint[MAX_ENDPOINT_LEN];
   char provider[16];
   provider_locality_t locality;
   catalog_health_t health;
   time_t last_success;
   time_t last_failure;
   char last_failure_class[64];
   int failure_streak;
   int active_count; /* in-flight delegates for this agent */
} provider_catalog_entry_t;

/* ---- Public API ---- */

/* Initialise the module-global catalog from an agent list.
 * Safe to call more than once; later calls replace the catalog state.
 * agents may be NULL with count=0 to produce an empty catalog. */
void provider_catalog_init(const agent_t *agents, int count);

/* Record that a call to agent_name succeeded. */
void provider_catalog_record_success(const char *agent_name);

/* Record that a call to agent_name failed.
 * failure_class is an opaque tag ("rate_limit", "unavailable", etc.);
 * may be NULL or empty. */
void provider_catalog_record_failure(const char *agent_name, const char *failure_class);

/* Query health state for a named agent.  Returns HEALTHY for unknown agents
 * so unknown names never block routing. */
catalog_health_t provider_catalog_get_health(const char *agent_name);

/* Query locality for a named agent. */
provider_locality_t provider_catalog_get_locality(const char *agent_name);

/* Return the locality-inferred concurrency limit for agent_name.
 * Returns 0 when no inference applies (caller should use its own default).
 * This value is advisory; explicit config overrides always win. */
int provider_catalog_inferred_concurrency(const char *agent_name);

/* Attempt to acquire one in-flight slot for agent_name.
 * max_parallel: the per-agent cap (from agent_t.max_parallel); 0 = unlimited.
 * Returns 1 if the slot was acquired, 0 if already at the cap. */
int provider_catalog_concurrent_acquire(const char *agent_name, int max_parallel);

/* Release a previously acquired in-flight slot for agent_name. */
void provider_catalog_concurrent_release(const char *agent_name);

/* Classify an endpoint URL as local / LAN / remote.
 * Pure function — no catalog state required. */
provider_locality_t provider_catalog_classify_endpoint(const char *endpoint);

/* Human-readable label for display. */
const char *provider_locality_label(provider_locality_t loc);
const char *catalog_health_label(catalog_health_t h);

/* Write a JSON array of catalog entries into buf (null-terminated).
 * Returns the number of bytes written (excluding the NUL), or -1 on
 * overflow. */
int provider_catalog_dump_json(char *buf, size_t cap);

#endif /* DEC_PROVIDER_CATALOG_H */
