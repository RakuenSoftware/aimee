#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/db2/db3_route.h>

#include "platform_test_util.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DB2_REF        29u
/* Deliberately outside the canonical module/client range: these identities
 * exist only in this isolated provider-conformance runtime. */
#define PROVIDER_A_REF 1001u
#define PROVIDER_B_REF 1002u
#define COLLISION_REF  1003u

typedef struct
{
   uint64_t operation_id;
   uint64_t generation;
   int effects;
   int duplicates;
} fake_provider_t;

static void pump(bus_host_t *host, pthread_mutex_t *lock)
{
   pthread_mutex_lock(lock);
   assert(bus_host_pump(host) > 0);
   pthread_mutex_unlock(lock);
}

static void wait_for_clients(bus_host_t *host, pthread_mutex_t *lock, uint32_t count)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int attempt = 0; attempt < 2000; ++attempt)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= count)
         return;
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for DB3 clients");
}

static void attach(const char *socket_path, uint32_t principal_ref, bus_client_t *client)
{
   int fd = -1;
   assert(bus_endpoint_connect(socket_path, &fd) == 0);
   assert(bus_client_attach_as(fd, client, 1, principal_ref) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&fd) == 0);
}

static uint32_t slot_for(bus_host_t *host, uint32_t principal_ref)
{
   for (uint32_t slot = 0; slot < host->cfg.max_slots; ++slot)
      if (host->slots[slot].in_use && host->slots[slot].principal_ref == principal_ref)
         return slot;
   assert(!"principal has no slot");
   return UINT32_MAX;
}

static void receive_apply(bus_client_t *client, fake_provider_t *state, uint64_t expected_seq)
{
   bus_event_t event;
   assert(bus_client_poll(client, &event) == BUS_CLIENT_OK);
   assert(event.frame.event_kind == AIMEE_DB3_EVENT_APPLY && event.frame.seq == expected_seq);
   aimee_db3_apply_t apply;
   assert(aimee_db3_apply_decode(event.payload, event.payload_len, &apply) == 0);
   if (state->operation_id == apply.operation_id && state->generation == apply.generation)
      state->duplicates++;
   else
   {
      state->operation_id = apply.operation_id;
      state->generation = apply.generation;
      state->effects++;
   }
}

int main(void)
{
   char directory[256];
   snprintf(directory, sizeof(directory), "%s/aimee-db3-bus-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   char socket_path[512], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   const uint32_t apply_kind[] = {AIMEE_DB3_EVENT_APPLY};
   const uint32_t search_kind[] = {AIMEE_DB3_EVENT_SEARCH};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = DB2_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .publish = apply_kind,
        .publish_count = 1,
        .request = search_kind,
        .request_count = 1},
       {.principal_class = 1,
        .principal_ref = PROVIDER_A_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .subscribe = apply_kind,
        .subscribe_count = 1,
        .serve = search_kind,
        .serve_count = 1},
       {.principal_class = 1,
        .principal_ref = PROVIDER_B_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .subscribe = apply_kind,
        .subscribe_count = 1},
       {.principal_class = 1,
        .principal_ref = COLLISION_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .serve = search_kind,
        .serve_count = 1},
   };
   bus_host_config_t host_config = {.max_slots = 8,
                                    .slot_size = 512,
                                    .inline_budget = 256,
                                    .queue_capacity = 16,
                                    .arena_size = 8192};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 8,
                                          .stale_after_ns = 5000000000ULL,
                                          .grants = grants,
                                          .grant_count = 4};
   bus_runtime_t *runtime = bus_runtime_start(&host, &lock, &runtime_config);
   assert(runtime != NULL);

   bus_client_t db2, provider_a, provider_b;
   attach(socket_path, PROVIDER_A_REF, &provider_a);
   attach(socket_path, PROVIDER_B_REF, &provider_b);
   attach(socket_path, DB2_REF, &db2);
   wait_for_clients(&host, &lock, 3);

   int denied_fd = -1;
   bus_client_t denied;
   assert(bus_endpoint_connect(socket_path, &denied_fd) == 0);
   assert(bus_client_attach_as(denied_fd, &denied, 1, COLLISION_REF) == BUS_CLIENT_DENIED);
   assert(denied.attach_status == BUS_ATTACH_DENIED_POLICY);
   assert(bus_endpoint_close(&denied_fd) == 0);

   pthread_mutex_lock(&lock);
   assert(bus_host_serve_kind(&host, slot_for(&host, PROVIDER_B_REF), AIMEE_DB3_EVENT_SEARCH) !=
          BUS_HOST_OK);
   pthread_mutex_unlock(&lock);

   aimee_db3_apply_t apply = {.operation_id = 1001,
                              .generation = 7,
                              .point_id = 41,
                              .kind = AIMEE_DB3_APPLY_UPSERT,
                              .collection = "memory",
                              .dimension = 3,
                              .vector = {0.1f, 0.2f, 0.3f}};
   uint8_t wire[256];
   size_t length = 0;
   assert(aimee_db3_apply_encode(&apply, wire, sizeof(wire), &length) == 0);
   assert(bus_client_publish(&db2, AIMEE_DB3_EVENT_APPLY, wire, (uint32_t)length) == BUS_CLIENT_OK);
   pump(&host, &lock);
   fake_provider_t state_a = {0}, state_b = {0};
   receive_apply(&provider_a, &state_a, 1);
   receive_apply(&provider_b, &state_b, 1);
   assert(state_a.effects == 1 && state_b.effects == 1);

   assert(bus_client_publish(&db2, AIMEE_DB3_EVENT_APPLY, wire, (uint32_t)length) == BUS_CLIENT_OK);
   pump(&host, &lock);
   receive_apply(&provider_a, &state_a, 2);
   receive_apply(&provider_b, &state_b, 2);
   assert(state_a.effects == 1 && state_b.effects == 1);
   assert(state_a.duplicates == 1 && state_b.duplicates == 1);

   aimee_db3_search_request_t request = {.request_id = 77,
                                         .required_generation = 7,
                                         .workspace = "workspace-a",
                                         .project = "project-a",
                                         .record_type = "memory",
                                         .dimension = 3,
                                         .top_k = 2,
                                         .vector = {0.3f, 0.2f, 0.1f}};
   assert(aimee_db3_search_request_encode(&request, wire, sizeof(wire), &length) == 0);
   assert(bus_client_request(&db2, AIMEE_DB3_EVENT_SEARCH, 9001, wire, (uint32_t)length) ==
          BUS_CLIENT_OK);
   pump(&host, &lock);

   bus_event_t event;
   assert(bus_client_poll(&provider_a, &event) == BUS_CLIENT_OK);
   assert((event.frame.hdr_flags & BUS_F_REQUEST) != 0 &&
          event.frame.event_kind == AIMEE_DB3_EVENT_SEARCH);
   uint64_t server_correlation = event.frame.correlation_id;
   aimee_db3_search_request_t decoded_request;
   assert(aimee_db3_search_request_decode(event.payload, event.payload_len, &decoded_request) == 0);
   assert(decoded_request.request_id == request.request_id);
   assert(bus_client_poll(&provider_b, &event) == BUS_CLIENT_EMPTY);

   aimee_db3_search_reply_t reply = {.request_id = request.request_id,
                                     .generation = request.required_generation,
                                     .count = 2,
                                     .candidates = {{41, 0.95}, {42, 0.75}}};
   assert(aimee_db3_search_reply_encode(&reply, wire, sizeof(wire), &length) == 0);
   assert(bus_client_reply(&provider_a, AIMEE_DB3_EVENT_SEARCH, server_correlation, wire,
                           (uint32_t)length) == BUS_CLIENT_OK);
   pump(&host, &lock);
   assert(bus_client_poll(&db2, &event) == BUS_CLIENT_OK);
   assert((event.frame.hdr_flags & BUS_F_REPLY) != 0 && event.frame.correlation_id == 9001);
   aimee_db3_search_reply_t decoded_reply;
   assert(aimee_db3_search_reply_decode(event.payload, event.payload_len, &decoded_reply) == 0);
   assert(aimee_db3_search_reply_validate(&request, &decoded_reply) == 0);
   assert(decoded_reply.count == 2 && decoded_reply.candidates[0].point_id == 41);

   bus_client_detach(&db2);
   bus_client_detach(&provider_a);
   bus_client_detach(&provider_b);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&lock);
   assert(rmdir(directory) == 0);
   puts("test_bus_db3: two write observers and one read server passed");
   return 0;
}
