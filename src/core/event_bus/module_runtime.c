#define _POSIX_C_SOURCE 200809L

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/module_runtime.h>

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODULE_MAX_INFLIGHT 16u

typedef struct
{
   int in_use;
   pthread_t thread;
   atomic_int cancelled;
   atomic_int done;
   uint32_t event_kind;
   uint64_t correlation_id;
   aimee_module_invocation_t invocation;
   aimee_module_handler_fn handler;
   void *user_data;
   uint8_t *request_body;
   uint32_t request_len;
   uint8_t *response_body;
   uint32_t response_capacity;
   uint32_t response_len;
   aimee_module_status_t status;
} module_work_t;

static volatile sig_atomic_t process_running = 1;

static uint64_t monotonic_now_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void process_signal(int signal_number)
{
   (void)signal_number;
   process_running = 0;
}

void aimee_module_process_stop(void)
{
   process_running = 0;
}

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   if (!invocation)
      return 1;
   const module_work_t *work = invocation->runtime_state;
   if (work && atomic_load_explicit(&work->cancelled, memory_order_acquire))
      return 1;
   uint64_t now = monotonic_now_ns();
   return now != 0 && aimee_module_deadline_expired(invocation->deadline_ns, now);
}

static uint32_t stage_for_kind(const aimee_module_process_config_t *config, uint32_t kind)
{
   for (size_t i = 0; i < config->stage_count; ++i)
      if (config->stages[i].event_kind == kind)
         return config->stages[i].stage_id;
   return 0;
}

static int status_valid(aimee_module_status_t status)
{
   return status >= AIMEE_MODULE_STATUS_OK && status <= AIMEE_MODULE_STATUS_INTERNAL;
}

static void *run_handler(void *argument)
{
   module_work_t *work = argument;
   work->response_len = 0;
   work->status =
       work->handler(&work->invocation, work->request_body, work->request_len, work->response_body,
                     work->response_capacity, &work->response_len, work->user_data);
   if (!status_valid(work->status) || work->response_len > work->response_capacity)
   {
      work->status = AIMEE_MODULE_STATUS_INTERNAL;
      work->response_len = 0;
   }
   if (atomic_load_explicit(&work->cancelled, memory_order_acquire))
   {
      work->status = AIMEE_MODULE_STATUS_CANCELLED;
      work->response_len = 0;
   }
   else
   {
      uint64_t now = monotonic_now_ns();
      if (now != 0 && aimee_module_deadline_expired(work->invocation.deadline_ns, now))
      {
         work->status = AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED;
         work->response_len = 0;
      }
   }
   atomic_store_explicit(&work->done, 1, memory_order_release);
   return NULL;
}

static void reply_result(bus_client_t *client, uint32_t kind, uint64_t correlation,
                         uint32_t stage_id, uint64_t trace_id, aimee_module_status_t status,
                         const uint8_t *body, uint32_t body_len)
{
   size_t total = (size_t)AIMEE_MODULE_MESSAGE_HEADER_LEN + body_len;
   if (total > client->reply.inline_budget)
   {
      status = AIMEE_MODULE_STATUS_INTERNAL;
      body = NULL;
      body_len = 0;
      total = AIMEE_MODULE_MESSAGE_HEADER_LEN;
   }
   uint8_t *payload = malloc(total);
   if (!payload)
      return;
   aimee_module_message_t reply = {.operation = AIMEE_MODULE_OP_RESULT,
                                   .status = (uint16_t)status,
                                   .stage_id = stage_id ? stage_id : 1u,
                                   .body_len = body_len,
                                   .trace_id = trace_id};
   if (aimee_module_message_encode(&reply, payload, total) != 0)
   {
      if (body_len)
         memcpy(payload + AIMEE_MODULE_MESSAGE_HEADER_LEN, body, body_len);
      (void)bus_client_reply(client, kind, correlation, payload, (uint32_t)total);
   }
   free(payload);
}

static module_work_t *find_work(module_work_t *work, uint64_t correlation)
{
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
      if (work[i].in_use && work[i].correlation_id == correlation)
         return &work[i];
   return NULL;
}

static module_work_t *free_work(module_work_t *work)
{
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
      if (!work[i].in_use)
         return &work[i];
   return NULL;
}

static void reap_work(bus_client_t *client, module_work_t *work)
{
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
   {
      module_work_t *item = &work[i];
      if (!item->in_use || !atomic_load_explicit(&item->done, memory_order_acquire))
         continue;
      (void)pthread_join(item->thread, NULL);
      reply_result(client, item->event_kind, item->correlation_id, item->invocation.stage_id,
                   item->invocation.trace_id, item->status, item->response_body,
                   item->response_len);
      free(item->request_body);
      free(item->response_body);
      memset(item, 0, sizeof *item);
   }
}

static void stop_work(module_work_t *work)
{
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
      if (work[i].in_use)
         atomic_store_explicit(&work[i].cancelled, 1, memory_order_release);
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
   {
      if (!work[i].in_use)
         continue;
      (void)pthread_join(work[i].thread, NULL);
      free(work[i].request_body);
      free(work[i].response_body);
   }
}

static void start_request(bus_client_t *client, const aimee_module_process_config_t *config,
                          module_work_t *work, const bus_event_t *event, uint64_t now)
{
   uint32_t expected_stage = stage_for_kind(config, event->frame.event_kind);
   aimee_module_message_t request;
   aimee_module_message_result_t decoded =
       aimee_module_message_decode(event->payload, event->payload_len, &request);
   if (expected_stage == 0 || decoded != AIMEE_MODULE_MESSAGE_OK ||
       request.operation != AIMEE_MODULE_OP_INVOKE || request.stage_id != expected_stage)
   {
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage, 0,
                   AIMEE_MODULE_STATUS_INVALID_REQUEST, NULL, 0);
      return;
   }
   if (now != 0 && aimee_module_deadline_expired(request.deadline_ns, now))
   {
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   request.trace_id, AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED, NULL, 0);
      return;
   }
   if (!config->handler)
   {
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   request.trace_id, AIMEE_MODULE_STATUS_CAPABILITY_ABSENT, NULL, 0);
      return;
   }

   module_work_t *item = free_work(work);
   if (!item)
   {
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   request.trace_id, AIMEE_MODULE_STATUS_INTERNAL, NULL, 0);
      return;
   }
   memset(item, 0, sizeof *item);
   uint32_t response_capacity = client->reply.inline_budget > AIMEE_MODULE_MESSAGE_HEADER_LEN
                                    ? client->reply.inline_budget - AIMEE_MODULE_MESSAGE_HEADER_LEN
                                    : 0;
   if (request.body_len)
   {
      item->request_body = malloc(request.body_len);
      if (!item->request_body)
         goto allocation_failed;
      memcpy(item->request_body, event->payload + AIMEE_MODULE_MESSAGE_HEADER_LEN,
             request.body_len);
   }
   if (response_capacity)
   {
      item->response_body = malloc(response_capacity);
      if (!item->response_body)
         goto allocation_failed;
   }
   item->in_use = 1;
   atomic_init(&item->cancelled, 0);
   atomic_init(&item->done, 0);
   item->event_kind = event->frame.event_kind;
   item->correlation_id = event->frame.correlation_id;
   item->invocation.stage_id = expected_stage;
   item->invocation.deadline_ns = request.deadline_ns;
   item->invocation.trace_id = request.trace_id;
   item->invocation.runtime_state = item;
   item->handler = config->handler;
   item->user_data = config->user_data;
   item->request_len = request.body_len;
   item->response_capacity = response_capacity;
   if (pthread_create(&item->thread, NULL, run_handler, item) == 0)
      return;

   item->in_use = 0;
allocation_failed:
   free(item->request_body);
   free(item->response_body);
   memset(item, 0, sizeof *item);
   reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                request.trace_id, AIMEE_MODULE_STATUS_INTERNAL, NULL, 0);
}

static int config_valid(const aimee_module_process_config_t *config)
{
   if (!config || !config->socket_path || !config->socket_path[0] || !config->module_name ||
       !config->module_name[0] || !config->stages || config->stage_count == 0 ||
       config->principal_class == 0 || config->principal_ref == 0)
      return 0;
   for (size_t i = 0; i < config->stage_count; ++i)
      if (config->stages[i].event_kind == 0 || config->stages[i].stage_id == 0)
         return 0;
   return 1;
}

int aimee_module_process_run(const aimee_module_process_config_t *config)
{
   if (!config_valid(config))
      return 2;
   process_running = 1;
   struct sigaction action;
   memset(&action, 0, sizeof action);
   action.sa_handler = process_signal;
   sigemptyset(&action.sa_mask);
   (void)sigaction(SIGINT, &action, NULL);
   (void)sigaction(SIGTERM, &action, NULL);

   int socket_fd = -1;
   bus_client_t client;
   if (bus_endpoint_connect(config->socket_path, &socket_fd) != 0 ||
       bus_client_attach_as(socket_fd, &client, config->principal_class, config->principal_ref) !=
           BUS_CLIENT_OK)
   {
      fprintf(stderr, "%s: event-bus attach failed\n", config->module_name);
      bus_endpoint_close(&socket_fd);
      return 1;
   }
   bus_endpoint_close(&socket_fd);

   module_work_t work[MODULE_MAX_INFLIGHT];
   memset(work, 0, sizeof work);
   while (process_running && !bus_client_epoch_changed(&client))
   {
      uint64_t now = monotonic_now_ns();
      if (now != 0)
         bus_client_heartbeat(&client, now);
      reap_work(&client, work);

      bus_event_t event;
      bus_client_result_t result = bus_client_poll(&client, &event);
      if (result == BUS_CLIENT_EPOCH)
         break;
      if (result == BUS_CLIENT_OK && (event.frame.hdr_flags & BUS_F_CANCEL))
      {
         module_work_t *item = find_work(work, event.frame.correlation_id);
         if (item)
            atomic_store_explicit(&item->cancelled, 1, memory_order_release);
      }
      else if (result == BUS_CLIENT_OK && (event.frame.hdr_flags & BUS_F_REQUEST))
      {
         start_request(&client, config, work, &event, now);
      }
      const struct timespec idle = {.tv_sec = 0, .tv_nsec = 1000000};
      nanosleep(&idle, NULL);
   }
   stop_work(work);
   bus_client_detach(&client);
   return 0;
}
