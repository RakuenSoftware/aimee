#define _GNU_SOURCE
#include <aimee/audit/obs_bus.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint64_t direct_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
/* Real credential-checked Unix module transport, no Native/data-store effects.
 * The fixed service context is preserved as data; it is not Human authority. */
#include "module_commands.h"
#include <ouroboros/client.h>
#include <ouroboros/endpoint.h>
#include <ouroboros/module_protocol.h>
#include <aimee/ir/aimee_ir.h>
#include <limits.h>
#include <sys/stat.h>
#define DIRECT_KIND    5896u
#define DIRECT_STAGE   8u
#define DIRECT_BLOCKED 8
static char direct_socket[108];
static pthread_t direct_server_thread;
static atomic_int direct_ready, direct_stop, direct_entered, direct_cancel_seen, direct_late_sent,
    direct_shutdown_mode;
static const char *const direct_raw[] = {"{\"wide\":18446744073709551615}",
                                         "{\"escaped\":\"safe\\u0000tail\"}",
                                         "{\"duplicate\":1,\"duplicate\":2}", "{\"malformed\":"};
static void direct_write32(unsigned char *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (unsigned char)(value >> (8 * i));
}
static uint32_t direct_read32(const unsigned char *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void direct_pause(void)
{
   const struct timespec d = {0, 1000000};
   nanosleep(&d, NULL);
}
static void direct_reply(ouro_client_t *client, uint64_t correlation, const char *json)
{
   unsigned char wire[1024];
   size_t n = strlen(json);
   assert(n + 12 + OURO_MODULE_MESSAGE_HEADER_LEN <= sizeof wire);
   direct_write32(wire + OURO_MODULE_MESSAGE_HEADER_LEN, 0x53504d43u);
   direct_write32(wire + OURO_MODULE_MESSAGE_HEADER_LEN + 4, 1);
   direct_write32(wire + OURO_MODULE_MESSAGE_HEADER_LEN + 8, (uint32_t)n);
   memcpy(wire + OURO_MODULE_MESSAGE_HEADER_LEN + 12, json, n);
   ouro_module_message_t message = {.operation = OURO_MODULE_OP_RESULT,
                                    .stage_id = DIRECT_STAGE,
                                    .body_len = (uint32_t)n + 12,
                                    .status = OURO_MODULE_STATUS_OK};
   assert(ouro_module_message_encode(&message, wire, sizeof wire));
   ouro_client_result_t result;
   for (unsigned i = 0;; ++i)
   {
      result = ouro_client_reply(client, DIRECT_KIND, correlation, wire,
                                 (uint32_t)n + 12 + OURO_MODULE_MESSAGE_HEADER_LEN);
      if (result != OURO_CLIENT_WOULD_BLOCK)
         break;
      assert(i < 1000);
      direct_pause();
   }
   assert(result == OURO_CLIENT_OK);
}
static void *direct_server(void *unused)
{
   (void)unused;
   int fd = -1;
   ouro_client_t client;
   assert(ouro_endpoint_connect(direct_socket, &fd) == 0);
   assert(ouro_client_attach_as(fd, &client, 1, 7) == OURO_CLIENT_OK);
   ouro_endpoint_close(&fd);
   uint64_t old[DIRECT_BLOCKED];
   size_t old_count = 0;
   atomic_store(&direct_ready, 1);
   while (!atomic_load(&direct_stop))
   {
      ouro_client_heartbeat(&client, direct_now_ns());
      ouro_event_t event;
      ouro_client_result_t polled = ouro_client_poll(&client, &event);
      if (polled == OURO_CLIENT_EMPTY)
      {
         direct_pause();
         continue;
      }
      if (polled == OURO_CLIENT_EPOCH && atomic_load(&direct_shutdown_mode))
         break;
      assert(polled == OURO_CLIENT_OK);
      if (event.frame.hdr_flags & OURO_F_CANCEL)
      {
         atomic_fetch_add(&direct_cancel_seen, 1);
         continue;
      }
      assert(event.frame.event_kind == DIRECT_KIND);
      ouro_module_message_t message;
      assert(ouro_module_message_decode(event.payload, event.payload_len, &message) ==
             OURO_MODULE_MESSAGE_OK);
      assert(message.operation == OURO_MODULE_OP_INVOKE && message.stage_id == DIRECT_STAGE &&
             message.trace_id == 0);
      const unsigned char *body = event.payload + OURO_MODULE_MESSAGE_HEADER_LEN;
      assert(message.body_len >= 20 && direct_read32(body) == 0x51504d43u &&
             direct_read32(body + 4) == 2);
      uint32_t verb = direct_read32(body + 8) & 65535, args = direct_read32(body + 12),
               ctx = direct_read32(body + 16);
      assert(verb == 7 && !memcmp(body + 20, "runtime", 7) &&
             (uint64_t)20 + verb + args + ctx == message.body_len);
      cJSON *a = cJSON_ParseWithLength((const char *)body + 20 + verb, args),
            *c = cJSON_ParseWithLength((const char *)body + 20 + verb + args, ctx);
      assert(a && c);
      assert(cJSON_IsTrue(cJSON_GetObjectItem(c, "authenticated")) &&
             cJSON_IsFalse(cJSON_GetObjectItem(c, "user_authority")));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(c, "principal")),
                     "fixed-operational-service"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(c, "transport_identity")),
                     "host:operational-test"));
      const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(a, "operation"));
      assert(op);
      if (!strcmp(op, "block"))
      {
         assert(old_count < DIRECT_BLOCKED);
         old[old_count++] = event.frame.correlation_id;
         atomic_fetch_add(&direct_entered, 1);
      }
      else
      {
         if (!strcmp(op, "after_cancel"))
         {
            for (size_t i = 0; i < old_count; ++i)
            {
               direct_reply(&client, old[i], "{\"marker\":\"OLD_MUST_NOT_CROSS\"}");
               atomic_fetch_add(&direct_late_sent, 1);
            }
            old_count = 0;
         }
         direct_reply(&client, event.frame.correlation_id,
                      !strncmp(op, "raw", 3) && op[3] >= '0' && op[3] <= '3'
                          ? direct_raw[op[3] - '0']
                          : "{\"marker\":\"NEW\",\"ok\":true}");
      }
      cJSON_Delete(a);
      cJSON_Delete(c);
   }
   ouro_client_detach(&client);
   return NULL;
}
void module_command_cancellation_setup(const char *home)
{
   char policy[512], path[600], exe[PATH_MAX];
   assert(snprintf(direct_socket, sizeof direct_socket, "%s/direct.sock", home) > 0);
   assert(snprintf(policy, sizeof policy, "%s/direct-policy", home) > 0);
   assert(mkdir(policy, 0700) == 0);
   assert(snprintf(path, sizeof path, "%s/server.grant", policy) > 0);
   assert(realpath("/proc/self/exe", exe));
   FILE *f = fopen(path, "w");
   assert(f);
   assert(fprintf(f,
                  "version=1\nprincipal_class=1\nprincipal_ref=7\nuid="
                  "self\nexecutable=%s\npublish=\nsubscribe=\nrequest=\nserve=%u\n",
                  exe, DIRECT_KIND) > 0);
   assert(fclose(f) == 0 && chmod(path, 0600) == 0);
   assert(obs_bus_configure_module_runtime(direct_socket, policy) == 0);
}
static cJSON *direct_context(void)
{
   return cJSON_Parse("{\"authenticated\":true,\"user_authority\":false,"
                      "\"principal\":\"fixed-operational-service\",\"transport_"
                      "identity\":\"host:operational-test\"}");
}
typedef struct
{
   cJSON *args, *context, *reply;
} direct_owned_t;
static void direct_owned_cleanup(void *opaque)
{
   direct_owned_t *o = opaque;
   cJSON_Delete(o->args);
   cJSON_Delete(o->context);
   cJSON_Delete(o->reply);
}
static void *direct_blocked_call(void *unused)
{
   (void)unused;
   direct_owned_t owned = {cJSON_Parse("{\"operation\":\"block\"}"), direct_context(), NULL};
   assert(owned.args && owned.context);
   pthread_cleanup_push(direct_owned_cleanup, &owned);
   owned.reply = aimee_module_command_call_context_timeout(DIRECT_KIND, DIRECT_STAGE, "runtime",
                                                           owned.args, owned.context, 30000);
   pthread_cleanup_pop(1);
   return NULL;
}
static cJSON *direct_call(const char *operation, int raw)
{
   cJSON *args = cJSON_CreateObject(), *context = direct_context();
   assert(args && context && cJSON_AddStringToObject(args, "operation", operation));
   cJSON *reply = raw ? aimee_module_command_call_raw_context_timeout(
                            DIRECT_KIND, DIRECT_STAGE, "runtime", args, context, 5000)
                      : aimee_module_command_call_context_timeout(DIRECT_KIND, DIRECT_STAGE,
                                                                  "runtime", args, context, 5000);
   cJSON_Delete(args);
   cJSON_Delete(context);
   return reply;
}
void module_command_cancellation_cases(void)
{
   assert(pthread_create(&direct_server_thread, NULL, direct_server, NULL) == 0);
   for (int i = 0; i < 3000 && !atomic_load(&direct_ready); ++i)
      direct_pause();
   assert(atomic_load(&direct_ready));
   pthread_t blocked[DIRECT_BLOCKED], waiter;
   for (int i = 0; i < DIRECT_BLOCKED; ++i)
      assert(pthread_create(&blocked[i], NULL, direct_blocked_call, NULL) == 0);
   for (int i = 0; i < 5000 && atomic_load(&direct_entered) != DIRECT_BLOCKED; ++i)
      direct_pause();
   assert(atomic_load(&direct_entered) == DIRECT_BLOCKED);
   /* This ninth caller is in the real pool condition wait, not a fake RPC. */
   assert(pthread_create(&waiter, NULL, direct_blocked_call, NULL) == 0);
   for (int i = 0; i < 30; ++i)
      direct_pause();
   uint64_t start = direct_now_ns();
   assert(pthread_cancel(waiter) == 0);
   void *result = NULL;
   assert(pthread_join(waiter, &result) == 0 && result == PTHREAD_CANCELED);
   assert(direct_now_ns() - start < 2000000000ULL);
   for (int i = 0; i < DIRECT_BLOCKED; ++i)
      assert(pthread_cancel(blocked[i]) == 0);
   for (int i = 0; i < DIRECT_BLOCKED; ++i)
   {
      result = NULL;
      assert(pthread_join(blocked[i], &result) == 0 && result == PTHREAD_CANCELED);
   }
   cJSON *reply = direct_call("after_cancel", 0);
   assert(reply && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "marker")), "NEW"));
   cJSON_Delete(reply);
   assert(atomic_load(&direct_late_sent) == DIRECT_BLOCKED);
   for (int i = 0; i < 4; i++)
   {
      char operation[8];
      snprintf(operation, sizeof operation, "raw%d", i);
      reply = direct_call(operation, 1);
      assert(cJSON_IsRaw(reply) && reply->valuestring &&
             !strcmp(reply->valuestring, direct_raw[i]));
      char *printed = cJSON_PrintUnformatted(reply);
      assert(printed && !strcmp(printed, direct_raw[i]));
      free(printed);
      cJSON *strict = aimee_ir_json_parse_frame(reply->valuestring, strlen(reply->valuestring));
      assert((strict != NULL) == (i == 0));
      cJSON_Delete(strict);
      cJSON_Delete(reply);
   }
   reply = direct_call("ordinary", 0);
   assert(reply && cJSON_IsTrue(cJSON_GetObjectItem(reply, "ok")));
   cJSON_Delete(reply);
   atomic_store(&direct_stop, 1);
   assert(pthread_join(direct_server_thread, NULL) == 0);
   puts("direct SDK: canceled RPC/poolwait, late-old isolation, raw bytes, "
        "normal parity passed");
}

/* Catalog discovery is deliberately outside this direct-route fixture. */
int mcp_osv_gate_blocks_argv(const char *name, int argc, const char *const argv[])
{
   (void)name;
   (void)argc;
   (void)argv;
   assert(!"direct API unexpectedly entered catalog discovery");
   return 1;
}

/* Stop must cooperatively finish a checked-out call before detaching its ring. */
void module_command_cancellation_shutdown(void)
{
   assert(pthread_create(&direct_server_thread, NULL, direct_server, NULL) == 0);
   for (int i = 0; i < 3000 && !atomic_load(&direct_ready); ++i)
      direct_pause();
   assert(atomic_load(&direct_ready));
   pthread_t blocked;
   assert(pthread_create(&blocked, NULL, direct_blocked_call, NULL) == 0);
   for (int i = 0; i < 3000 && !atomic_load(&direct_entered); ++i)
      direct_pause();
   assert(atomic_load(&direct_entered) == 1);
   atomic_store(&direct_shutdown_mode, 1);
   uint64_t start = direct_now_ns();
   obs_bus_stop();
   void *result = (void *)1;
   assert(pthread_join(blocked, &result) == 0 && result == NULL);
   assert(direct_now_ns() - start < 3000000000ULL);
   atomic_store(&direct_stop, 1);
   assert(pthread_join(direct_server_thread, NULL) == 0);
   puts("direct SDK: shutdown finishes in-flight transport before releasing source buffers");
}
