#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/db2/db3_contract.h>

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The two provider principals come from the band reserved for DB3 vector
 * providers, because the Go router refuses one that does not: ObserveCapabilities
 * calls ValidateProviderRef before it will route to anybody. These were 1001 and
 * 1002 until the band was introduced, after which the router refused both and no
 * route ever deployed -- the grants here were still valid, so the bus let the
 * providers attach and the failure only showed up as a route query that never
 * came good. The band itself is db3_provider_principal_ref_band in
 * tests/baselines/modules/canonical-inventory.yaml, pinned to db3.ProviderRefFirst
 * by server-go/db3/principal_inventory_test.go. These two grants only have to
 * AGREE with what the Go test attaches as; if they ever stop agreeing, the bus
 * refuses the attach and the test says which principal it was, rather than
 * failing later on a route that never deploys. */
/* The caller is POSTGRES, not DB2.
 *
 * Only one principal holds a real grant to speak to a vector database, and it
 * is the postgres module: every other DB operation reaches the vector store by
 * going to postgres first. Granting the caller role to DB2 here would let a
 * fixture prove a path that no deployment is allowed to take. 28 is postgres. */
#define CALLER_REF     28u
#define PROVIDER_A_REF 456u
#define PROVIDER_B_REF 457u
/* The control principal only REQUESTS routes; it is not a provider, so it is not
 * band-checked and deliberately sits outside the band. */
#define CONTROL_REF 1003u

static volatile sig_atomic_t stopping;

static void stop_now(int signal_number)
{
   (void)signal_number;
   stopping = 1;
}

int main(int argc, char **argv)
{
   if (argc != 3 && argc != 4)
   {
      fprintf(stderr, "usage: %s SOCKET GO_TEST_EXECUTABLE [PROVIDER_EXECUTABLE]\n", argv[0]);
      return 2;
   }
   /* With a provider executable, provider A is a SEPARATE PROCESS rather than a
    * goroutine inside the Go test. The grant checks the peer's /proc/<pid>/exe
    * against this exact string, so the shipped provider binary can only attach
    * when the grant names it -- which is the whole point of running it this way:
    * every other test of the split runs the provider in-process, where no grant
    * is ever checked and no deployment is ever exercised. */
   const char *provider_a_executable = argc == 4 ? argv[3] : argv[2];
   signal(SIGTERM, stop_now);
   signal(SIGINT, stop_now);

   const uint32_t caller_publish[] = {AIMEE_DB3_EVENT_APPLY};
   const uint32_t caller_subscribe[] = {AIMEE_DB3_EVENT_CAPABILITIES, AIMEE_DB3_EVENT_APPLIED};
   const uint32_t caller_request[] = {AIMEE_DB3_EVENT_SEARCH};
   const uint32_t caller_serve[] = {AIMEE_DB3_EVENT_ROUTE};
   const uint32_t provider_publish[] = {AIMEE_DB3_EVENT_CAPABILITIES, AIMEE_DB3_EVENT_APPLIED};
   const uint32_t provider_subscribe[] = {AIMEE_DB3_EVENT_APPLY};
   const uint32_t provider_serve[] = {AIMEE_DB3_EVENT_SEARCH};
   const uint32_t control_request[] = {AIMEE_DB3_EVENT_ROUTE};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = argv[2],
        .publish = caller_publish,
        .publish_count = 1,
        .subscribe = caller_subscribe,
        .subscribe_count = 2,
        .request = caller_request,
        .request_count = 1,
        .serve = caller_serve,
        .serve_count = 1},
       {.principal_class = 1,
        .principal_ref = PROVIDER_A_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = provider_a_executable,
        .publish = provider_publish,
        .publish_count = 2,
        .subscribe = provider_subscribe,
        .subscribe_count = 1,
        .serve = provider_serve,
        .serve_count = 1},
       {.principal_class = 1,
        .principal_ref = PROVIDER_B_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = argv[2],
        .publish = provider_publish,
        .publish_count = 2,
        .subscribe = provider_subscribe,
        .subscribe_count = 1},
       {.principal_class = 1,
        .principal_ref = CONTROL_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = argv[2],
        .request = control_request,
        .request_count = 1},
   };
   bus_host_config_t host_config = {.max_slots = 8,
                                    .slot_size = 256,
                                    .inline_budget = 128,
                                    .queue_capacity = 64,
                                    .arena_size = 1u << 20};
   bus_host_t host;
   if (bus_host_create(&host, &host_config, NULL, NULL) != BUS_HOST_OK)
      return 1;
   pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = argv[1],
                                          .socket_mode = 0600,
                                          .backlog = 8,
                                          .stale_after_ns = 5000000000ULL,
                                          .grants = grants,
                                          .grant_count = sizeof(grants) / sizeof(grants[0])};
   bus_runtime_t *runtime = bus_runtime_start(&host, &lock, &runtime_config);
   if (!runtime)
   {
      bus_host_destroy(&host);
      pthread_mutex_destroy(&lock);
      return 1;
   }

   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 500000};
   while (!stopping)
   {
      pthread_mutex_lock(&lock);
      (void)bus_host_pump(&host);
      pthread_mutex_unlock(&lock);
      nanosleep(&pause, NULL);
   }

   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&lock);
   return 0;
}
