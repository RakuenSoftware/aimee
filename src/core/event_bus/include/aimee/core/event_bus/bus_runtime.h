#ifndef AIMEE_CORE_EVENT_BUS_RUNTIME_H
#define AIMEE_CORE_EVENT_BUS_RUNTIME_H 1

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include <aimee/core/event_bus/bus_host.h>

#define BUS_RUNTIME_SELF_UID UINT32_MAX

typedef struct
{
   uint32_t principal_class;
   uint32_t principal_ref;
   uint32_t uid; /* BUS_RUNTIME_SELF_UID means the daemon's effective uid */
   const char *executable; /* canonical absolute /proc/<pid>/exe target */
   const uint32_t *publish;
   size_t publish_count;
   const uint32_t *subscribe;
   size_t subscribe_count;
   const uint32_t *request;
   size_t request_count;
   const uint32_t *serve;
   size_t serve_count;
} bus_runtime_grant_t;

typedef struct bus_runtime bus_runtime_t;
typedef struct bus_runtime_policy bus_runtime_policy_t;

typedef struct
{
   const char *socket_path;
   unsigned socket_mode;
   int backlog;
   uint64_t stale_after_ns;
   const bus_runtime_grant_t *grants;
   size_t grant_count;
} bus_runtime_config_t;

/* Start the authenticated local module endpoint. host_lock must guard every
 * other access to host while this runtime is active; the accept thread uses the
 * same lock for admission, capability binding, and descriptor grant. */
bus_runtime_t *bus_runtime_start(bus_host_t *host, pthread_mutex_t *host_lock,
                                 const bus_runtime_config_t *config);
void bus_runtime_stop(bus_runtime_t **runtime);

/* Call periodically while holding host_lock. Updates the host heartbeat and
 * reaps clients whose own heartbeat stopped advancing. */
uint32_t bus_runtime_maintain(bus_runtime_t *runtime, uint64_t now_ns);
uint64_t bus_runtime_monotonic_ns(void);

/* Strict deployment policy: one *.grant file per installed module. A missing
 * directory is an empty policy (the listener still runs and denies all). */
int bus_runtime_policy_load_dir(const char *directory, bus_runtime_policy_t **out);
void bus_runtime_policy_free(bus_runtime_policy_t **policy);
const bus_runtime_grant_t *bus_runtime_policy_grants(const bus_runtime_policy_t *policy,
                                                     size_t *count_out);

#endif
