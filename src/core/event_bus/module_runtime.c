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

typedef struct
{
   int in_use;
   uint32_t event_kind;
   uint64_t correlation_id;
   aimee_module_message_t message;
   uint8_t *body;
   uint32_t body_len;
   uint32_t body_capacity;
} module_assembly_t;

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
   else if (work->status != AIMEE_MODULE_STATUS_OK)
      work->response_len = 0;
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
   if (body_len > AIMEE_MODULE_MESSAGE_MAX_BODY || (body_len > 0 && !body) ||
       status != AIMEE_MODULE_STATUS_OK)
   {
      if (body_len > AIMEE_MODULE_MESSAGE_MAX_BODY || (body_len > 0 && !body))
         status = AIMEE_MODULE_STATUS_INTERNAL;
      body = NULL;
      body_len = 0;
   }
   if (client->reply.inline_budget <= AIMEE_MODULE_MESSAGE_HEADER_LEN)
      return;
   uint32_t chunk_capacity = client->reply.inline_budget - AIMEE_MODULE_MESSAGE_HEADER_LEN;
   uint8_t *payload = malloc(client->reply.inline_budget);
   if (!payload)
      return;
   uint32_t offset = 0;
   int first = 1;
   const struct timespec retry = {.tv_sec = 0, .tv_nsec = 1000000};
   while (first || offset < body_len)
   {
      first = 0;
      uint32_t remaining = body_len - offset;
      uint32_t part = remaining < chunk_capacity ? remaining : chunk_capacity;
      int more = remaining > part;
      size_t total = (size_t)AIMEE_MODULE_MESSAGE_HEADER_LEN + part;
      aimee_module_message_t reply = {.operation = AIMEE_MODULE_OP_RESULT,
                                      .status = (uint16_t)status,
                                      .stage_id = stage_id ? stage_id : 1u,
                                      .body_len = part,
                                      .trace_id = trace_id};
      if (aimee_module_message_encode(&reply, payload, total) == 0)
         break;
      if (part)
         memcpy(payload + AIMEE_MODULE_MESSAGE_HEADER_LEN, body + offset, part);
      for (;;)
      {
         bus_client_result_t sent =
             bus_client_reply_fragment(client, kind, correlation, payload, (uint32_t)total, more);
         if (sent == BUS_CLIENT_OK)
            break;
         if (sent != BUS_CLIENT_WOULD_BLOCK || !process_running || bus_client_epoch_changed(client))
         {
            free(payload);
            return;
         }
         uint64_t now = monotonic_now_ns();
         if (now)
            bus_client_heartbeat(client, now);
         nanosleep(&retry, NULL);
      }
      offset += part;
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

static module_assembly_t *find_assembly(module_assembly_t *assembly, uint64_t correlation)
{
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
      if (assembly[i].in_use && assembly[i].correlation_id == correlation)
         return &assembly[i];
   return NULL;
}

static module_assembly_t *free_assembly_slot(module_assembly_t *assembly)
{
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
      if (!assembly[i].in_use)
         return &assembly[i];
   return NULL;
}

static void discard_assembly(module_assembly_t *assembly)
{
   if (!assembly)
      return;
   free(assembly->body);
   memset(assembly, 0, sizeof(*assembly));
}

static int append_assembly(module_assembly_t *assembly, const uint8_t *body, uint32_t body_len)
{
   if (body_len == 0)
      return 0;
   if (!body || assembly->body_len > AIMEE_MODULE_MESSAGE_MAX_BODY - body_len)
      return -1;
   uint32_t needed = assembly->body_len + body_len;
   if (needed > assembly->body_capacity)
   {
      uint32_t capacity = assembly->body_capacity ? assembly->body_capacity : 4096u;
      while (capacity < needed)
      {
         if (capacity > AIMEE_MODULE_MESSAGE_MAX_BODY / 2u)
         {
            capacity = AIMEE_MODULE_MESSAGE_MAX_BODY;
            break;
         }
         capacity *= 2u;
      }
      uint8_t *grown = realloc(assembly->body, capacity);
      if (!grown)
         return -1;
      assembly->body = grown;
      assembly->body_capacity = capacity;
   }
   memcpy(assembly->body + assembly->body_len, body, body_len);
   assembly->body_len = needed;
   return 0;
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
                          module_work_t *work, module_assembly_t *assemblies,
                          const bus_event_t *event, uint64_t now)
{
   uint32_t expected_stage = stage_for_kind(config, event->frame.event_kind);
   aimee_module_message_t request;
   aimee_module_message_result_t decoded =
       aimee_module_message_decode(event->payload, event->payload_len, &request);
   module_assembly_t *assembly = find_assembly(assemblies, event->frame.correlation_id);
   if (expected_stage == 0 || decoded != AIMEE_MODULE_MESSAGE_OK ||
       request.operation != AIMEE_MODULE_OP_INVOKE || request.stage_id != expected_stage ||
       find_work(work, event->frame.correlation_id) != NULL ||
       (assembly && (assembly->event_kind != event->frame.event_kind ||
                     assembly->message.stage_id != request.stage_id ||
                     assembly->message.deadline_ns != request.deadline_ns ||
                     assembly->message.trace_id != request.trace_id)))
   {
      discard_assembly(assembly);
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage, 0,
                   AIMEE_MODULE_STATUS_INVALID_REQUEST, NULL, 0);
      return;
   }

   if (!assembly)
   {
      assembly = free_assembly_slot(assemblies);
      if (!assembly)
      {
         reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                      request.trace_id, AIMEE_MODULE_STATUS_INTERNAL, NULL, 0);
         return;
      }
      memset(assembly, 0, sizeof(*assembly));
      assembly->in_use = 1;
      assembly->event_kind = event->frame.event_kind;
      assembly->correlation_id = event->frame.correlation_id;
      assembly->message = request;
   }
   if (append_assembly(assembly, event->payload + AIMEE_MODULE_MESSAGE_HEADER_LEN,
                       request.body_len) != 0)
   {
      uint64_t trace_id = request.trace_id;
      discard_assembly(assembly);
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   trace_id, AIMEE_MODULE_STATUS_INTERNAL, NULL, 0);
      return;
   }
   if (event->frame.hdr_flags & BUS_F_MORE)
      return;

   /* The final fragment transfers the complete body to one worker. */
   uint8_t *request_body = assembly->body;
   uint32_t request_len = assembly->body_len;
   assembly->body = NULL;
   discard_assembly(assembly);

   if (now != 0 && aimee_module_deadline_expired(request.deadline_ns, now))
   {
      free(request_body);
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   request.trace_id, AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED, NULL, 0);
      return;
   }
   if (!config->handler)
   {
      free(request_body);
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   request.trace_id, AIMEE_MODULE_STATUS_CAPABILITY_ABSENT, NULL, 0);
      return;
   }

   module_work_t *item = free_work(work);
   if (!item)
   {
      free(request_body);
      reply_result(client, event->frame.event_kind, event->frame.correlation_id, expected_stage,
                   request.trace_id, AIMEE_MODULE_STATUS_INTERNAL, NULL, 0);
      return;
   }
   memset(item, 0, sizeof(*item));
   item->request_body = request_body;
   item->response_body = malloc(AIMEE_MODULE_MESSAGE_MAX_BODY);
   if (!item->response_body)
      goto allocation_failed;
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
   item->request_len = request_len;
   item->response_capacity = AIMEE_MODULE_MESSAGE_MAX_BODY;
   if (pthread_create(&item->thread, NULL, run_handler, item) == 0)
      return;

   item->in_use = 0;
allocation_failed:
   free(item->request_body);
   free(item->response_body);
   memset(item, 0, sizeof(*item));
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
   module_assembly_t assemblies[MODULE_MAX_INFLIGHT];
   memset(assemblies, 0, sizeof assemblies);
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
         discard_assembly(find_assembly(assemblies, event.frame.correlation_id));
      }
      else if (result == BUS_CLIENT_OK && (event.frame.hdr_flags & BUS_F_REQUEST))
      {
         start_request(&client, config, work, assemblies, &event, now);
      }
      const struct timespec idle = {.tv_sec = 0, .tv_nsec = 1000000};
      nanosleep(&idle, NULL);
   }
   stop_work(work);
   for (size_t i = 0; i < MODULE_MAX_INFLIGHT; ++i)
      discard_assembly(&assemblies[i]);
   bus_client_detach(&client);
   return 0;
}
