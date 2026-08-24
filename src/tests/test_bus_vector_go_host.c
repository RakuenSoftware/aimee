#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/db2/vector_contract.h>

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DB2_REF        29u
#define PROVIDER_A_REF 1001u
#define PROVIDER_B_REF 1002u
#define CONTROL_REF    1003u

static volatile sig_atomic_t stopping;

static void stop_now(int signal_number)
{
   (void)signal_number;
   stopping = 1;
}

int main(int argc, char **argv)
{
   if (argc != 3)
   {
      fprintf(stderr, "usage: %s SOCKET GO_TEST_EXECUTABLE\n", argv[0]);
      return 2;
   }
   signal(SIGTERM, stop_now);
   signal(SIGINT, stop_now);

   const uint32_t db2_publish[] = {AIMEE_VECTOR_EVENT_APPLY};
   const uint32_t db2_subscribe[] = {AIMEE_VECTOR_EVENT_CAPABILITIES, AIMEE_VECTOR_EVENT_APPLIED};
   const uint32_t db2_request[] = {AIMEE_VECTOR_EVENT_SEARCH};
   const uint32_t db2_serve[] = {AIMEE_VECTOR_EVENT_ROUTE};
   const uint32_t provider_publish[] = {AIMEE_VECTOR_EVENT_CAPABILITIES,
                                        AIMEE_VECTOR_EVENT_APPLIED};
   const uint32_t provider_subscribe[] = {AIMEE_VECTOR_EVENT_APPLY};
   const uint32_t provider_serve[] = {AIMEE_VECTOR_EVENT_SEARCH};
   const uint32_t control_request[] = {AIMEE_VECTOR_EVENT_ROUTE};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = DB2_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = argv[2],
        .publish = db2_publish,
        .publish_count = 1,
        .subscribe = db2_subscribe,
        .subscribe_count = 2,
        .request = db2_request,
        .request_count = 1,
        .serve = db2_serve,
        .serve_count = 1},
       {.principal_class = 1,
        .principal_ref = PROVIDER_A_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = argv[2],
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
