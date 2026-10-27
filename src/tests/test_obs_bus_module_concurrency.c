/* test_obs_bus_module_concurrency.c: concurrent C->module calls must not
 * serialize behind one another.
 *
 * A module call is synchronous and holds its client for the whole request and
 * reply. While every caller shared one client, a long stage blocked every other
 * module call in the process. That is not a throughput concern, it is a
 * deadlock: a roundtable review held the client for minutes, and the module
 * running that review called back into this same server to launch its seats --
 * a callback that needed a client and could never get one. The review waited on
 * itself and only a remote timeout broke it.
 *
 * These tests pin the two properties that make that impossible:
 *   1. Calls proceed concurrently rather than one at a time.
 *   2. A caller never waits past its own deadline for a client, so pool
 *      exhaustion is reported as a deadline instead of becoming a hang. */

#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <aimee/audit/obs_bus.h>
#include "log.h"
#include <ouroboros/module_client.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

void audit_log_open(void);

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* No module is attached, so every call resolves to capability_absent. That is
 * exactly what this test wants: the outcome is uninteresting, the concurrency
 * of getting there is the point. */
#define ABSENT_KIND  5889u
#define ABSENT_STAGE 1u

enum
{
   CALLERS = 8,
   CALLS_PER_CALLER = 50
};

static void *call_many(void *arg)
{
   (void)arg;
   for (int i = 0; i < CALLS_PER_CALLER; ++i)
   {
      char response[16];
      uint32_t response_len = 0;
      ouro_module_call_result_t result =
          obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 1, now_ns() + 5000000000ULL, "probe", 5,
                              response, sizeof response, &response_len, NULL, NULL);
      assert(result == OURO_MODULE_CALL_CAPABILITY_ABSENT);
   }
   return NULL;
}

/* The property is overlap, not speed.
 *
 * While one client was shared, its mutex spanned the whole request and reply,
 * so no two calls could ever be in flight at once -- maximum overlap was
 * exactly 1 by construction. Comparing wall time to the sum of call durations
 * does NOT detect that: a queued caller's own measurement includes the time it
 * spent waiting, so the sum inflates under serialization and the comparison
 * stays true either way. Counting genuinely overlapping intervals is the
 * difference that cannot be faked. */
static void test_calls_overlap(void)
{
   pthread_t threads[CALLERS];
   for (int i = 0; i < CALLERS; ++i)
      assert(pthread_create(&threads[i], NULL, call_many, NULL) == 0);
   for (int i = 0; i < CALLERS; ++i)
      pthread_join(threads[i], NULL);

   /* Ask the bus how many calls ever held a client at once. Measuring this from
    * outside does not work: a caller blocked waiting for a client is inside the
    * call the whole time, so wall-clock spans overlap just as much when every
    * call is serialized -- more, in fact, because everyone is queued. */
   int occupancy = obs_bus_module_peak_concurrency();
   printf("  %d calls across %d callers, peak client occupancy=%d\n", CALLERS * CALLS_PER_CALLER,
          CALLERS, occupancy);
   assert(occupancy > 1);
}

/* A caller that cannot get a client within its own deadline must be told so.
 * Waiting past the deadline is precisely the behaviour that turned a busy pool
 * into an unbounded hang. */
static void test_expired_deadline_does_not_block(void)
{
   char response[16];
   uint32_t response_len = 0;
   uint64_t start = now_ns();
   ouro_module_call_result_t result =
       obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 1, now_ns() - 1, "probe", 5, response,
                           sizeof response, &response_len, NULL, NULL);
   uint64_t elapsed = now_ns() - start;
   assert(result == OURO_MODULE_CALL_DEADLINE_EXCEEDED);
   /* "Returned promptly" is the claim; a second is orders of magnitude more
    * than any bookkeeping here needs and still catches a blocking wait. */
   assert(elapsed < 1000000000ULL);
}

/* Capture/privacy checks below inspect the production tap directly. */
#include "../modules/audit/obs_bus.c"

static ouro_module_status_t source_lookup_callback(const ouro_module_invocation_t *invocation,
                                                   const uint8_t *request, uint32_t length,
                                                   uint8_t *response, uint32_t capacity,
                                                   uint32_t *response_length, void *context)
{
   (void)context;
   assert(!ouro_module_invocation_cancelled(invocation));
   assert(length == 6 && !memcmp(request, "nested", 6) && capacity >= 6);
   memcpy(response, "source", 6);
   *response_length = 6;
   return OURO_MODULE_STATUS_OK;
}
static ouro_module_status_t embedded_callback(const ouro_module_invocation_t *invocation,
                                              const uint8_t *request, uint32_t length,
                                              uint8_t *response, uint32_t capacity,
                                              uint32_t *response_length, void *context)
{
   (void)context;
   assert(!ouro_module_invocation_cancelled(invocation));
   char nested[16];
   uint32_t nested_length = 0;
   assert(obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 9, now_ns() + 1000000000ULL, "nested", 6,
                              nested, sizeof nested, &nested_length, NULL,
                              NULL) == OURO_MODULE_CALL_CAPABILITY_ABSENT);
   /* A source resolver may call another independently owned bus stage while
    * this host worker remains admitted. The central pump must keep progressing. */
   assert(obs_bus_module_call(6680, 24, 10, now_ns() + 1000000000ULL, "nested", 6, nested,
                              sizeof nested, &nested_length, NULL, NULL) == OURO_MODULE_CALL_OK);
   assert(nested_length == 6 && !memcmp(nested, "source", 6));
   assert(length <= capacity);
   memcpy(response, request, length);
   *response_length = length;
   return OURO_MODULE_STATUS_OK;
}
static obs_bus_module_service_t *test_embedded_callback_service(void)
{
   static const ouro_module_stage_t stages[] = {{6679, 23}};
   obs_bus_module_service_t *service = NULL, *duplicate = NULL, *source = NULL;
   static const ouro_module_stage_t source_stages[] = {{6680, 24}};
   assert(!obs_bus_module_service_start(source_stages, 1, source_lookup_callback, NULL, &source));
   assert(!obs_bus_module_service_start(stages, 1, embedded_callback, NULL, &service));
   assert(obs_bus_module_service_start(stages, 1, embedded_callback, NULL, &duplicate) < 0 &&
          !duplicate);
   char response[16];
   uint32_t length = 0;
   assert(obs_bus_module_call(6679, 23, 7, now_ns() + 2000000000ULL, "typed", 5, response,
                              sizeof response, &length, NULL, NULL) == OURO_MODULE_CALL_OK);
   assert(length == 5 && !memcmp(response, "typed", 5));
   obs_bus_module_service_stop(&service);
   obs_bus_module_service_stop(&source);
   assert(!service && !source);
   assert(obs_bus_module_call(6679, 23, 8, now_ns() + 1000000000ULL, "typed", 5, response,
                              sizeof response, &length, NULL,
                              NULL) == OURO_MODULE_CALL_CAPABILITY_ABSENT);
   assert(!obs_bus_module_service_start(stages, 1, embedded_callback, NULL, &service));
   return service; /* obs_bus_stop must safely retire it before its pump/owners. */
}

static int scope_contains(const uint8_t *bytes, size_t length, const char *needle, size_t n)
{
   for (size_t i = 0; n <= length && i <= length - n; i++)
      if (!memcmp(bytes + i, needle, n))
         return 1;
   return 0;
}
static unsigned scope_receipts;
static int scope_structural_sink(const char *role, const char *principal, const char *action,
                                 const char *subject, const char *verdict, const char *detail,
                                 void *context)
{
   (void)role;
   (void)principal;
   (void)subject;
   (void)verdict;
   (void)context;
   assert(!strcmp(action, "bus.module.request") || !strcmp(action, "bus.module.reply"));
   assert(strstr(detail, "event_kind") && strstr(detail, "principal_ref"));
   assert(!strstr(detail, "execution_scope") && !strstr(detail, "0123456789abcdef"));
   scope_receipts++;
   return 0;
}
static void scope_frame(uint32_t kind, uint64_t sequence, int reply, int more, const char *body,
                        size_t length, uint32_t correlation)
{
   uint8_t payload[512];
   ouro_module_message_t message = {.operation =
                                        reply ? OURO_MODULE_OP_RESULT : OURO_MODULE_OP_INVOKE,
                                    .status = OURO_MODULE_STATUS_OK,
                                    .stage_id = 1,
                                    .body_len = (uint32_t)length,
                                    .trace_id = 9};
   assert(ouro_module_message_encode(&message, payload, sizeof payload) ==
          OURO_MODULE_MESSAGE_HEADER_LEN);
   memcpy(payload + OURO_MODULE_MESSAGE_HEADER_LEN, body, length);
   ouro_frame_t frame = {.wire_version = OURO_WIRE_VERSION,
                         .event_kind = kind,
                         .seq = sequence,
                         .hdr_flags = (reply ? OURO_F_REPLY : OURO_F_REQUEST) | OURO_F_INLINE |
                                      (more ? OURO_F_MORE : 0),
                         .principal_ref = 76,
                         .correlation_id = correlation,
                         .payload_ref = OURO_WIRE_HDR_LEN,
                         .payload_len = (uint32_t)(OURO_MODULE_MESSAGE_HEADER_LEN + length)};
   governance_tap(NULL, &frame, payload, frame.payload_len);
}
static void test_scope_secrets_never_enter_capture(void)
{
   /* Live secret-bearing stages; the removed trigger-source stage is not a protocol. */
   static const uint32_t kinds[] = {6657, 6679, 9218, 9474, 11792, 11266};
   static const char secret[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   char body[128];
   snprintf(body, sizeof body, "{\"execution_scope\":\"%s\"}", secret);
   ouro_capture_init(&g.capture, 1, 1, 1);
   atomic_store(&g.capture_state, CAPTURE_OK);
   sinks.durable = scope_structural_sink;
   uint64_t sequence = 1;
   for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
   {
      for (int reply = 0; reply < 2; reply++)
      {
         size_t split =
             39; // The key and only part of the secret are in the first real AMOD fragment.
         scope_frame(kinds[i], sequence++, reply, 1, body, split, (uint32_t)i * 2 + reply + 1);
         scope_frame(kinds[i], sequence++, reply, 0, body + split, strlen(body) - split,
                     (uint32_t)i * 2 + reply + 1);
      }
   }
   assert(g.capture.len == 0); // Neither key-bearing fragments nor secret-only tails persisted.
   scope_frame(5889, sequence++, 0, 0, "ordinary-body", 13, 99);
   scope_frame(5889, sequence++, 1, 0, "ordinary-reply", 14, 99);
   assert(g.capture.len && scope_contains(g.capture.buf, g.capture.len, "ordinary-body", 13));
   assert(scope_contains(g.capture.buf, g.capture.len, "ordinary-reply", 14));
   assert(!scope_contains(g.capture.buf, g.capture.len, secret, strlen(secret)));
   assert(!scope_contains(g.capture.buf, g.capture.len, secret, 32));
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-scope-capture-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0 && write(fd, g.capture.buf, g.capture.len) == (ssize_t)g.capture.len);
   assert(lseek(fd, 0, SEEK_SET) == 0);
   uint8_t *persisted = malloc(g.capture.len);
   assert(persisted && read(fd, persisted, g.capture.len) == (ssize_t)g.capture.len);
   assert(scope_contains(persisted, g.capture.len, "ordinary-body", 13));
   assert(!scope_contains(persisted, g.capture.len, secret, 32));
   free(persisted);
   close(fd);
   assert(!unlink(path));
   ouro_capture_report_t report = ouro_capture_read(g.capture.buf, g.capture.len, NULL, NULL);
   assert(report.status == OURO_CAPTURE_OPEN && report.records == 2);
   /* Four live ledger protocols produce request/reply receipts (8); the two
    * capture-only protocols suppress bodies without ledger receipts. The two
    * ordinary memory frames add 2. The retired trigger-source stage adds none. */
   assert(flush_pending_durable() == 10 && scope_receipts == 10);
   assert(!atomic_load(&g.dropped) && !atomic_load(&g.durable_pending_count));
   ouro_capture_free(&g.capture);
   puts("  auth-bearing fragmented request/reply bodies suppressed; structural WORM and ordinary "
        "capture retained: ok");
}

static ouro_module_status_t owned_service_echo(const ouro_module_invocation_t *invocation,
                                               const uint8_t *body, uint32_t length,
                                               uint8_t *output, uint32_t capacity,
                                               uint32_t *out_len, void *context)
{
   (void)context;
   assert(invocation->source_principal_ref == 76);
   assert(length <= capacity);
   memcpy(output, body, length);
   *out_len = length;
   return OURO_MODULE_STATUS_OK;
}
static int connect_owned_listener_fixture(const char *path, int *fd)
{
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   assert(strlen(path) < sizeof address.sun_path);
   strcpy(address.sun_path, path);
   *fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
   return *fd < 0 ? -1 : connect(*fd, (struct sockaddr *)&address, sizeof address);
}
static void test_owned_service_after_external_policy(void)
{
   char home[256], socket[108], policy[300], grant[340], executable[4096];
   assert(snprintf(home, sizeof home, "%s/aimee-ownedbus-XXXXXX", platform_tmpdir()) <
          (int)sizeof home);
   assert(mkdtemp(home) && !setenv("AIMEE_HOME", home, 1));
   assert(snprintf(socket, sizeof socket, "%s/module.sock", home) < (int)sizeof socket);
   snprintf(policy, sizeof policy, "%s/policy", home);
   assert(!mkdir(policy, 0700));
   snprintf(grant, sizeof grant, "%s/requester.grant", policy);
   ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
   assert(n > 0);
   executable[n] = 0;
   FILE *f = fopen(grant, "w");
   assert(f);
   fprintf(f,
           "version=1\nprincipal_class=1\nprincipal_ref=76\nuid=self\nexecutable=%s\npublish="
           "\nsubscribe=\nrequest=6679\nserve=\n",
           executable);
   assert(!fclose(f));
   audit_log_open();
   assert(!obs_bus_configure_module_runtime(socket, policy));
   assert(!obs_bus_start());
   static const ouro_module_stage_t stages[] = {{6679, 23}};
   obs_bus_module_service_t *service = NULL;
   assert(!obs_bus_module_service_start(stages, 1, owned_service_echo, NULL, &service));
   assert(service && g.host.slots[service->bus.reply.handle_id].principal_class == 0 &&
          g.host.slots[service->bus.reply.handle_id].principal_ref == 0);
   /* Even a genuine private pair cannot mint an external requester identity. */
   int pair[2];
   assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
   struct serve_arg argument = {.fd = pair[1], .owned = 1};
   pthread_t thread;
   ouro_admit_fn admission = g.host.admit;
   ouro_attach_status_t (*hook)(void *, struct ouro_host *, uint32_t,
                                const ouro_attach_request_t *) = g.host.attach_hook;
   pthread_mutex_lock(&g.host_lock);
   assert(!pthread_create(&thread, NULL, serve_thread, &argument));
   ouro_client_t impersonator;
   assert(ouro_client_attach_as(pair[0], &impersonator, 1, 76) == OURO_CLIENT_DENIED);
   assert(!pthread_join(thread, NULL));
   assert(g.host.admit == admission && g.host.attach_hook == hook);
   pthread_mutex_unlock(&g.host_lock);
   close(pair[0]);
   close(pair[1]);
   int fd = -1;
   ouro_client_t external;
   assert(!connect_owned_listener_fixture(socket, &fd));
   assert(ouro_client_attach_as(fd, &external, 0, 0) == OURO_CLIENT_DENIED);
   close(fd);
   assert(!connect_owned_listener_fixture(socket, &fd));
   assert(ouro_client_attach_as(fd, &external, 1, 77) == OURO_CLIENT_DENIED);
   close(fd);
   assert(!connect_owned_listener_fixture(socket, &fd));
   assert(ouro_client_attach_as(fd, &external, 1, 76) == OURO_CLIENT_OK);
   close(fd);
   ouro_module_client_t caller;
   assert(!ouro_module_client_init(&caller, &external));
   char output[32];
   uint32_t length = 0;
   assert(ouro_module_client_call(&caller, 6679, 23, 1, now_ns() + 2000000000ULL, "owned", 5,
                                  output, sizeof output, &length, NULL,
                                  NULL) == OURO_MODULE_CALL_OK);
   assert(length == 5 && !memcmp(output, "owned", 5));
   ouro_module_client_destroy(&caller);
   ouro_client_detach(&external);
   obs_bus_module_service_stop(&service);
   assert(!service);
   obs_bus_stop();
   puts("  configured external listener: private host service starts; external zero/forged "
        "principals denied; granted requester works: ok");
}

/* A production KB durability sink reaches PostgreSQL through this same bus.
 * Calling it on the pump thread makes even a typed refusal time out. */
static atomic_int durable_ready;
static atomic_int durable_writes;
static int durable_module_sink(const char *role, const char *principal, const char *action,
                               const char *subject, const char *verdict, const char *detail,
                               void *context)
{
   (void)role;
   (void)principal;
   (void)subject;
   (void)verdict;
   (void)detail;
   (void)context;
   if (!atomic_load(&durable_ready))
      return -1;
   char reply[16];
   uint32_t length = 0;
   assert(obs_bus_module_call(11267, 3, 1, now_ns() + 1000000000ULL, "probe", 5, reply,
                              sizeof reply, &length, NULL,
                              NULL) == OURO_MODULE_CALL_CAPABILITY_ABSENT);
   if (strcmp(action, "test.provider.audit") == 0)
      atomic_fetch_add(&durable_writes, 1);
   return 0;
}

int main(void)
{
   pid_t owned_child = fork();
   assert(owned_child >= 0);
   if (!owned_child)
   {
      test_owned_service_after_external_policy();
      exit(0);
   }
   int owned_status = 0;
   assert(waitpid(owned_child, &owned_status, 0) == owned_child && WIFEXITED(owned_status) &&
          WEXITSTATUS(owned_status) == 0);
   pid_t capture_child = fork();
   assert(capture_child >= 0);
   if (!capture_child)
   {
      test_scope_secrets_never_enter_capture();
      exit(0);
   }
   int capture_status = 0;
   assert(waitpid(capture_child, &capture_status, 0) == capture_child &&
          WIFEXITED(capture_status) && WEXITSTATUS(capture_status) == 0);
   printf("obs_bus_module_concurrency:\n");
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busconc-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();
   assert(obs_bus_set_durable_sink(durable_module_sink, NULL) == 0);
   if (obs_bus_start() != 0)
   {
      fprintf(stderr, "FAIL: obs_bus_start\n");
      return 1;
   }

   obs_bus_emit_durable_event("test.provider.audit", "startup", "ok", "{}");
   obs_bus_module_service_t *service = test_embedded_callback_service();
   test_calls_overlap();
   test_expired_deadline_does_not_block();

   atomic_store(&durable_ready, 1);
   obs_bus_flush();
   assert(atomic_load(&durable_writes) == 1);
   for (int i = 0; i < 8; i++)
      obs_bus_emit_durable_event("test.provider.audit", "shutdown", "ok", "{}");
   obs_bus_stop();
   assert(atomic_load(&durable_writes) == 9);
   obs_bus_module_service_stop(&service);
   assert(!service);
   assert(obs_bus_dropped() == 0);
   printf("ok\n");
   return 0;
}
