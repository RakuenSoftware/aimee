#define _GNU_SOURCE
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_KIND 4100U

static bus_client_result_t attach(const char *path, uint32_t principal_ref, bus_client_t *client)
{
   int fd = -1;
   assert(bus_endpoint_connect(path, &fd) == 0);
   bus_client_result_t result = bus_client_attach_as(fd, client, 1, principal_ref);
   assert(bus_endpoint_close(&fd) == 0);
   return result;
}

int main(void)
{
   char directory[] = "/tmp/aimee-bus-runtime-XXXXXX";
   assert(mkdtemp(directory) != NULL);
   char socket_path[PATH_MAX], policy_path[PATH_MAX], grant_path[PATH_MAX], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/module.sock", directory) > 0);
   assert(snprintf(policy_path, sizeof(policy_path), "%s/policy", directory) > 0);
   assert(snprintf(grant_path, sizeof(grant_path), "%s/test.grant", policy_path) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);
   assert(mkdir(policy_path, 0700) == 0);

   FILE *manifest = fopen(grant_path, "w");
   assert(manifest != NULL);
   assert(fprintf(manifest,
                  "version=1\nprincipal_class=1\nprincipal_ref=7\nuid=self\n"
                  "executable=%s\npublish=%u\nsubscribe=\nrequest=\nserve=\n",
                  executable, TEST_KIND) > 0);
   assert(fclose(manifest) == 0);
   bus_runtime_policy_t *parsed = NULL;
   assert(bus_runtime_policy_load_dir(policy_path, &parsed) == 0);
   size_t parsed_count = 0;
   const bus_runtime_grant_t *parsed_grants = bus_runtime_policy_grants(parsed, &parsed_count);
   assert(parsed_grants != NULL && parsed_count == 1);
   assert(parsed_grants[0].principal_ref == 7 && parsed_grants[0].publish_count == 1);
   bus_runtime_policy_free(&parsed);

   uint32_t publisher_kinds[] = {TEST_KIND};
   uint32_t subscriber_kinds[] = {TEST_KIND};
   bus_runtime_grant_t grants[] = {{.principal_class = 1,
                                    .principal_ref = 7,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = executable,
                                    .publish = publisher_kinds,
                                    .publish_count = 1},
                                   {.principal_class = 1,
                                    .principal_ref = 8,
                                    .uid = BUS_RUNTIME_SELF_UID,
                                    .executable = executable,
                                    .subscribe = subscriber_kinds,
                                    .subscribe_count = 1}};

   bus_host_config_t host_config = {.max_slots = 8,
                                    .slot_size = 512,
                                    .inline_budget = 400,
                                    .queue_capacity = 8,
                                    .arena_size = 16384};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 8,
                                          .stale_after_ns = 1000000000ULL,
                                          .grants = grants,
                                          .grant_count = 2};
   bus_runtime_t *runtime = bus_runtime_start(&host, &host_lock, &runtime_config);
   assert(runtime != NULL);

   bus_client_t publisher, subscriber, denied;
   assert(attach(socket_path, 7, &publisher) == BUS_CLIENT_OK);
   assert(attach(socket_path, 8, &subscriber) == BUS_CLIENT_OK);
   assert(attach(socket_path, 99, &denied) == BUS_CLIENT_DENIED);

   static const char payload[] = "authorized";
   assert(bus_client_publish(&publisher, TEST_KIND, payload, sizeof(payload)) == BUS_CLIENT_OK);
   pthread_mutex_lock(&host_lock);
   assert(bus_host_pump(&host) == 1);
   pthread_mutex_unlock(&host_lock);
   bus_event_t event;
   assert(bus_client_poll(&subscriber, &event) == BUS_CLIENT_OK);
   assert(event.frame.event_kind == TEST_KIND && event.payload_len == sizeof(payload));
   assert(event.frame.principal_ref == 7 && event.frame.src_handle == publisher.reply.handle_id);
   assert(memcmp(event.payload, payload, sizeof(payload)) == 0);

   assert(bus_client_publish(&publisher, TEST_KIND + 1U, NULL, 0) == BUS_CLIENT_OK);
   pthread_mutex_lock(&host_lock);
   assert(bus_host_pump(&host) == 0);
   pthread_mutex_unlock(&host_lock);
   assert(bus_client_poll(&publisher, &event) == BUS_CLIENT_OK);
   assert(event.frame.event_kind == BUS_KIND_CAPABILITY_DENIED);

   uint64_t now = bus_runtime_monotonic_ns();
   bus_client_heartbeat(&publisher, now);
   bus_client_heartbeat(&subscriber, now);
   pthread_mutex_lock(&host_lock);
   assert(bus_runtime_maintain(runtime, now) == 0);
   pthread_mutex_unlock(&host_lock);

   bus_client_detach(&publisher);
   bus_client_detach(&subscriber);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   assert(unlink(grant_path) == 0);
   assert(rmdir(policy_path) == 0);
   assert(rmdir(directory) == 0);
   puts("bus runtime: authenticated attach and least-privilege routing passed");
   return 0;
}
