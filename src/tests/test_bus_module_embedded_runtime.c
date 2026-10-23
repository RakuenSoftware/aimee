#define _POSIX_C_SOURCE 200809L
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
static bus_host_t host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int pump_stop;
static const struct timespec pause_one = {0, 1000000};
static void *pump(void *unused)
{
   (void)unused;
   while (!atomic_load(&pump_stop))
   {
      pthread_mutex_lock(&lock);
      bus_host_pump(&host);
      pthread_mutex_unlock(&lock);
      nanosleep(&pause_one, NULL);
   }
   return NULL;
}
typedef struct
{
   int fd;
} attach_arg;
static void *attach_host(void *data)
{
   attach_arg *a = data;
   pthread_mutex_lock(&lock);
   assert(bus_host_serve_attach(&host, a->fd) == BUS_HOST_OK);
   pthread_mutex_unlock(&lock);
   return NULL;
}
static void attach(bus_client_t *client, uint32_t principal)
{
   int fd[2];
   assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fd));
   attach_arg a = {fd[1]};
   pthread_t t;
   assert(!pthread_create(&t, NULL, attach_host, &a));
   assert((principal ? bus_client_attach_as(fd[0], client, 1, principal)
                     : bus_client_attach(fd[0], client)) == BUS_CLIENT_OK);
   pthread_join(t, NULL);
   close(fd[0]);
   close(fd[1]);
}
typedef struct
{
   bus_client_t bus;
   aimee_module_process_config_t config;
   atomic_int stop, entered, deadline_seen;
   uint32_t expected_handle;
   int result;
} service;
static int stopping(void *data)
{
   return atomic_load(&((service *)data)->stop);
}
static aimee_module_status_t handle(const aimee_module_invocation_t *inv, const uint8_t *req,
                                    uint32_t len, uint8_t *out, uint32_t cap, uint32_t *outlen,
                                    void *data)
{
   service *s = data;
   assert(inv->source_principal_ref == 76);
   assert(inv->source_handle == s->expected_handle);
   assert(aimee_module_invocation_cancel_flag(inv));
   if (len == 8 && !memcmp(req, "deadline", 8))
   {
      const atomic_int *flag = aimee_module_invocation_cancel_flag(inv);
      while (!atomic_load(flag))
         nanosleep(&pause_one, NULL);
      atomic_store(&s->deadline_seen, atomic_load(flag));
      return AIMEE_MODULE_STATUS_OK;
   }
   if (len == 6 && !memcmp(req, "cancel", 6))
   {
      atomic_store(&s->entered, 1);
      while (!aimee_module_invocation_cancelled(inv))
         nanosleep(&pause_one, NULL);
      return AIMEE_MODULE_STATUS_OK;
   }
   assert(len <= cap);
   memcpy(out, req, len);
   *outlen = len;
   return AIMEE_MODULE_STATUS_OK;
}
static void *serve(void *data)
{
   service *s = data;
   s->result = aimee_module_service_run(&s->bus, &s->config, stopping, s);
   return NULL;
}
static uint64_t now(void)
{
   struct timespec t;
   assert(!clock_gettime(CLOCK_MONOTONIC, &t));
   return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
typedef struct
{
   aimee_module_client_t *client;
   uint32_t kind;
   aimee_module_call_result_t result;
} call;
static void *call_cancel(void *data)
{
   call *c = data;
   char out[8];
   uint32_t len = 0;
   c->result = aimee_module_client_call(c->client, c->kind, 23, 90, now() + 5000000000ULL, "cancel",
                                        6, out, sizeof out, &len, NULL, NULL);
   return NULL;
}
int main(void)
{
   bus_host_config_t cfg = {.max_slots = 8,
                            .slot_size = 4096,
                            .inline_budget = 512,
                            .queue_capacity = 16,
                            .arena_size = 4 * 1024 * 1024};
   assert(bus_host_create(&host, &cfg, NULL, NULL) == BUS_HOST_OK);
   bus_client_t caller;
   attach(&caller, 76);
   aimee_module_client_t client;
   assert(!aimee_module_client_init(&client, &caller));
   struct sigaction before, after;
   assert(!sigaction(SIGINT, NULL, &before));
   service a = {0}, b = {0};
   attach(&a.bus, 0);
   attach(&b.bus, 0);
   a.expected_handle = b.expected_handle = caller.reply.handle_id;
   static const aimee_module_stage_t sa[] = {{6679, 23}}, sb[] = {{6680, 23}};
   a.config = (aimee_module_process_config_t){
       .stages = sa, .stage_count = 1, .handler = handle, .user_data = &a};
   b.config = (aimee_module_process_config_t){
       .stages = sb, .stage_count = 1, .handler = handle, .user_data = &b};
   assert(bus_host_serve_kind(&host, a.bus.reply.handle_id, 6679) == BUS_HOST_OK);
   assert(bus_host_serve_kind(&host, b.bus.reply.handle_id, 6680) == BUS_HOST_OK);
   pthread_t pt, at, bt;
   assert(!pthread_create(&pt, NULL, pump, NULL));
   assert(!pthread_create(&at, NULL, serve, &a));
   assert(!pthread_create(&bt, NULL, serve, &b));
   uint32_t n = 0;
   char *in = malloc(128 * 1024), *out = malloc(128 * 1024);
   assert(in && out);
   memset(in, 'x', 128 * 1024);
   assert(aimee_module_client_call(&client, 6679, 23, 1, now() + 5000000000ULL, in, 128 * 1024, out,
                                   128 * 1024, &n, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(n == 128 * 1024 && !memcmp(in, out, n));
   assert(!sigaction(SIGINT, NULL, &after) && before.sa_handler == after.sa_handler);
   (void)aimee_module_client_call(&client, 6679, 23, 89, now() + 30000000ULL, "deadline", 8, out,
                                  128 * 1024, &n, NULL, NULL);
   for (int i = 0; i < 1000 && !atomic_load(&a.deadline_seen); i++)
      nanosleep(&pause_one, NULL);
   assert(atomic_load(&a.deadline_seen) == 2);
   call c = {&client, 6679, AIMEE_MODULE_CALL_INTERNAL};
   pthread_t ct;
   assert(!pthread_create(&ct, NULL, call_cancel, &c));
   for (int i = 0; i < 2000 && !atomic_load(&a.entered); i++)
      nanosleep(&pause_one, NULL);
   assert(atomic_load(&a.entered));
   atomic_store(&a.stop, 1);
   pthread_join(at, NULL);
   pthread_join(ct, NULL);
   assert(!a.result && c.result == AIMEE_MODULE_CALL_CANCELLED);
   assert(aimee_module_client_call(&client, 6680, 23, 2, now() + 5000000000ULL, "alive", 5, out,
                                   128 * 1024, &n, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(n == 5 && !memcmp(out, "alive", 5));
   atomic_store(&b.stop, 1);
   pthread_join(bt, NULL);
   assert(!b.result);
   atomic_store(&pump_stop, 1);
   pthread_join(pt, NULL);
   aimee_module_client_destroy(&client);
   bus_client_detach(&caller);
   bus_client_detach(&a.bus);
   bus_client_detach(&b.bus);
   bus_host_destroy(&host);
   free(in);
   free(out);
   puts("embedded module runtime: fragments, stamped identity, independent stop, joined "
        "cancellation, signals PASS");
   return 0;
}
