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

#include <aimee/audit/obs_bus.h>
#include "log.h"
#include <aimee/core/event_bus/module_client.h>
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
      aimee_module_call_result_t result =
          obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 1, now_ns() + 5000000000ULL, "probe", 5,
                              response, sizeof response, &response_len, NULL, NULL);
      assert(result == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
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
   aimee_module_call_result_t result =
       obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 1, now_ns() - 1, "probe", 5, response,
                           sizeof response, &response_len, NULL, NULL);
   uint64_t elapsed = now_ns() - start;
   assert(result == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   /* "Returned promptly" is the claim; a second is orders of magnitude more
    * than any bookkeeping here needs and still catches a blocking wait. */
   assert(elapsed < 1000000000ULL);
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
                              NULL) == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
   if (strcmp(action, "test.provider.audit") == 0)
      atomic_fetch_add(&durable_writes, 1);
   return 0;
}

/* Freeze the actual consumer between polling and dispatch. A flush must wait
 * for this handoff, rather than treating a polled event as persisted work. */
static pthread_mutex_t handoff_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handoff_changed = PTHREAD_COND_INITIALIZER;
static int handoff_armed, handoff_polled, handoff_release;
static int flush_waiting, flush_returned;
static atomic_int handoff_persisted;

static _Thread_local int handoff_flush_thread;
static int handoff_nanosleep(const struct timespec *request, struct timespec *remaining)
{
   if (handoff_flush_thread)
   {
      pthread_mutex_lock(&handoff_lock);
      flush_waiting = 1;
      pthread_cond_broadcast(&handoff_changed);
      pthread_mutex_unlock(&handoff_lock);
   }
   return nanosleep(request, remaining);
}

static void handoff_audit_action_log(const char *actor, const char *tool, const char *hash,
                                     const char *command, const char *mode, const char *reason,
                                     const char *verdict, long long task_id)
{
   pthread_mutex_lock(&handoff_lock);
   if (handoff_armed && !strcmp(tool, "test.handoff"))
   {
      handoff_polled = 1;
      pthread_cond_broadcast(&handoff_changed);
      while (!handoff_release)
         pthread_cond_wait(&handoff_changed, &handoff_lock);
   }
   pthread_mutex_unlock(&handoff_lock);
   audit_action_log(actor, tool, hash, command, mode, reason, verdict, task_id);
   if (!strcmp(tool, "test.handoff"))
      atomic_fetch_add(&handoff_persisted, 1);
}

/* Exercise the production consumer and flush with only its existing audit
 * sink and wait primitive intercepted. No shipped test hooks are required. */
#define audit_action_log handoff_audit_action_log
#define nanosleep        handoff_nanosleep
#include "../modules/audit/obs_bus.c"
#undef nanosleep
#undef audit_action_log

static void wait_for_handoff(int *condition)
{
   struct timespec limit;
   clock_gettime(CLOCK_REALTIME, &limit);
   limit.tv_sec += 3;
   while (!*condition)
      assert(pthread_cond_timedwait(&handoff_changed, &handoff_lock, &limit) == 0);
}

static void *flush_handoff(void *unused)
{
   (void)unused;
   pthread_mutex_lock(&handoff_lock);
   handoff_flush_thread = 1;
   pthread_mutex_unlock(&handoff_lock);
   obs_bus_flush();
   pthread_mutex_lock(&handoff_lock);
   flush_returned = 1;
   pthread_cond_broadcast(&handoff_changed);
   pthread_mutex_unlock(&handoff_lock);
   return NULL;
}

static void test_flush_waits_for_handoff(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-handoff-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) && !setenv("AIMEE_HOME", home, 1));
   audit_log_open();
   assert(!obs_bus_start());
   obs_bus_flush();
   pthread_mutex_lock(&handoff_lock);
   handoff_armed = 1;
   pthread_mutex_unlock(&handoff_lock);
   obs_bus_emit("operator", "test.handoff", "args", "owned", "mode", "ok", "allow", 0);
   pthread_mutex_lock(&handoff_lock);
   wait_for_handoff(&handoff_polled);
   pthread_mutex_unlock(&handoff_lock);
   pthread_t thread;
   assert(!pthread_create(&thread, NULL, flush_handoff, NULL));
   pthread_mutex_lock(&handoff_lock);
   struct timespec limit;
   clock_gettime(CLOCK_REALTIME, &limit);
   limit.tv_sec += 3;
   while (!flush_waiting && !flush_returned)
      assert(!pthread_cond_timedwait(&handoff_changed, &handoff_lock, &limit));
   /* Old publication order returns here before the sink has seen the event. */
   assert(flush_waiting && !flush_returned && !atomic_load(&handoff_persisted));
   handoff_release = 1;
   pthread_cond_broadcast(&handoff_changed);
   pthread_mutex_unlock(&handoff_lock);
   assert(!pthread_join(thread, NULL));
   assert(flush_returned && atomic_load(&handoff_persisted) == 1);
   assert(obs_bus_written() == 1 && !obs_bus_dropped());
   obs_bus_stop();
   puts("  flush waits for consumer handoff and persistence: ok");
}

static atomic_int batch_ready, batch_calls, batch_rows, batch_attempts, largest_batch;
static char expected[139][32];
static int batch_sink(const obs_bus_durable_record_t *rows, size_t count, void *ctx)
{
   (void)ctx;
   assert(count && count <= OBS_BUS_DURABLE_BATCH_MAX);
   atomic_fetch_add(&batch_attempts, 1);
   if (!atomic_load(&batch_ready))
      return -1;
   if ((int)count > atomic_load(&largest_batch))
      atomic_store(&largest_batch, (int)count);
   int offset = atomic_load(&batch_rows);
   for (size_t i = 0; i < count; i++)
   {
      assert(!strcmp(rows[i].action, "test.batch"));
      assert(!strcmp(rows[i].subject, expected[offset + (int)i]));
   }
   /* A producer appends while the callback owns borrowed prefix views. */
   if (offset == 0)
      obs_bus_emit_durable_event("test.batch", expected[130], "ok", "{}");
   atomic_fetch_add(&batch_calls, 1);
   atomic_fetch_add(&batch_rows, (int)count);
   return 0;
}
static void test_batch_retry(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busbatch-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) && !setenv("AIMEE_HOME", home, 1));
   audit_log_open();
   assert(obs_bus_set_durable_batch_sink(NULL, batch_sink, NULL) < 0);
   assert(!obs_bus_set_durable_batch_sink(durable_module_sink, batch_sink, NULL));
   assert(!obs_bus_start());
   assert(obs_bus_set_durable_batch_sink(durable_module_sink, batch_sink, NULL) < 0);
   /* A synchronous action still calls the single sink, even with batching on. */
   atomic_store(&durable_ready, 1);
   assert(!obs_bus_commit_action("operator", "sync.test", "args", "command", "mode", "ok", "allow",
                                 0));
   assert(!atomic_load(&batch_attempts));

   for (int i = 0; i < 139; i++)
      snprintf(expected[i], sizeof expected[i], "fifo-%d", i);
   for (int i = 0; i < 130; i++)
      obs_bus_emit_durable_event("test.batch", expected[i], "ok", "{}");
   for (int i = 0; i < 2000 && !atomic_load(&batch_attempts); i++)
      usleep(1000);
   assert(atomic_load(&batch_attempts) && !atomic_load(&batch_rows));
   atomic_store(&batch_ready, 1);
   obs_bus_flush();
   /* The callback publishes after the first flush snapshots its target. */
   obs_bus_flush();
   assert(atomic_load(&batch_rows) == 131 && atomic_load(&batch_calls) >= 3);
   assert(obs_bus_written() == 131 && !obs_bus_dropped());
   assert(atomic_load(&largest_batch) > 1);
   for (int i = 131; i < 139; i++)
      obs_bus_emit_durable_event("test.batch", expected[i], "ok", "{}");
   obs_bus_stop();
   assert(atomic_load(&batch_rows) == 139 && obs_bus_written() == 139 && !obs_bus_dropped());
   puts("  queued batch FIFO/retry/concurrent enqueue: ok");
}

static aimee_module_status_t source_lookup_callback(const aimee_module_invocation_t *invocation,
                                                    const uint8_t *request, uint32_t length,
                                                    uint8_t *response, uint32_t capacity,
                                                    uint32_t *response_length, void *context)
{
   (void)context;
   assert(!aimee_module_invocation_cancelled(invocation));
   assert(length == 6 && !memcmp(request, "nested", 6) && capacity >= 6);
   memcpy(response, "source", 6);
   *response_length = 6;
   return AIMEE_MODULE_STATUS_OK;
}
static aimee_module_status_t embedded_callback(const aimee_module_invocation_t *invocation,
                                               const uint8_t *request, uint32_t length,
                                               uint8_t *response, uint32_t capacity,
                                               uint32_t *response_length, void *context)
{
   (void)context;
   assert(!aimee_module_invocation_cancelled(invocation));
   char nested[16];
   uint32_t nested_length = 0;
   assert(obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 9, now_ns() + 1000000000ULL, "nested", 6,
                              nested, sizeof nested, &nested_length, NULL,
                              NULL) == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
   /* A source resolver may call another independently owned bus stage while
    * this host worker remains admitted. The central pump must keep progressing. */
   assert(obs_bus_module_call(6680, 24, 10, now_ns() + 1000000000ULL, "nested", 6, nested,
                              sizeof nested, &nested_length, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(nested_length == 6 && !memcmp(nested, "source", 6));
   assert(length <= capacity);
   memcpy(response, request, length);
   *response_length = length;
   return AIMEE_MODULE_STATUS_OK;
}
static obs_bus_module_service_t *test_embedded_callback_service(void)
{
   static const aimee_module_stage_t stages[] = {{6679, 23}};
   obs_bus_module_service_t *service = NULL, *duplicate = NULL, *source = NULL;
   static const aimee_module_stage_t source_stages[] = {{6680, 24}};
   assert(!obs_bus_module_service_start(source_stages, 1, source_lookup_callback, NULL, &source));
   assert(!obs_bus_module_service_start(stages, 1, embedded_callback, NULL, &service));
   assert(obs_bus_module_service_start(stages, 1, embedded_callback, NULL, &duplicate) < 0 &&
          !duplicate);
   char response[16];
   uint32_t length = 0;
   assert(obs_bus_module_call(6679, 23, 7, now_ns() + 2000000000ULL, "typed", 5, response,
                              sizeof response, &length, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(length == 5 && !memcmp(response, "typed", 5));
   obs_bus_module_service_stop(&service);
   obs_bus_module_service_stop(&source);
   assert(!service && !source);
   assert(obs_bus_module_call(6679, 23, 8, now_ns() + 1000000000ULL, "typed", 5, response,
                              sizeof response, &length, NULL,
                              NULL) == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
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
   aimee_module_message_t message = {.operation =
                                         reply ? AIMEE_MODULE_OP_RESULT : AIMEE_MODULE_OP_INVOKE,
                                     .status = AIMEE_MODULE_STATUS_OK,
                                     .stage_id = 1,
                                     .body_len = (uint32_t)length,
                                     .trace_id = 9};
   assert(aimee_module_message_encode(&message, payload, sizeof payload) ==
          AIMEE_MODULE_MESSAGE_HEADER_LEN);
   memcpy(payload + AIMEE_MODULE_MESSAGE_HEADER_LEN, body, length);
   bus_frame_t frame = {.wire_version = BUS_WIRE_VERSION,
                        .event_kind = kind,
                        .seq = sequence,
                        .hdr_flags = (reply ? BUS_F_REPLY : BUS_F_REQUEST) | BUS_F_INLINE |
                                     (more ? BUS_F_MORE : 0),
                        .principal_ref = 76,
                        .correlation_id = correlation,
                        .payload_ref = BUS_WIRE_HDR_LEN,
                        .payload_len = (uint32_t)(AIMEE_MODULE_MESSAGE_HEADER_LEN + length)};
   governance_tap(NULL, &frame, payload, frame.payload_len);
}
static void test_scope_secrets_never_enter_capture(void)
{
   static const uint32_t kinds[] = {6657, 6679, 6680, 9218, 9474, 11792, 11266};
   static const char secret[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   char body[128];
   snprintf(body, sizeof body, "{\"execution_scope\":\"%s\"}", secret);
   bus_capture_init(&g.capture, 1, 1, 1);
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
   bus_capture_report_t report = bus_capture_read(g.capture.buf, g.capture.len, NULL, NULL);
   assert(report.status == BUS_CAPTURE_OPEN && report.records == 2);
   assert(flush_pending_durable() == 12 && scope_receipts == 12);
   assert(!atomic_load(&g.dropped) && !atomic_load(&g.durable_pending_count));
   bus_capture_free(&g.capture);
   puts("  auth-bearing fragmented request/reply bodies suppressed; structural WORM and ordinary "
        "capture retained: ok");
}

int main(void)
{
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
   pid_t handoff_child = fork();
   assert(handoff_child >= 0);
   if (!handoff_child)
   {
      test_flush_waits_for_handoff();
      exit(0);
   }
   int handoff_status = 0;
   assert(waitpid(handoff_child, &handoff_status, 0) == handoff_child &&
          WIFEXITED(handoff_status) && WEXITSTATUS(handoff_status) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      test_batch_retry();
      exit(0);
   }
   int status = 0;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
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
   assert(!obs_bus_set_durable_batch_sink(durable_module_sink, batch_sink, NULL));
   /* The legacy setter must clear a previously configured optional batch. */
   assert(obs_bus_set_durable_sink(durable_module_sink, NULL) == 0);
   if (obs_bus_start() != 0)
   {
      fprintf(stderr, "FAIL: obs_bus_start\n");
      return 1;
   }

   obs_bus_module_service_t *service = test_embedded_callback_service();
   obs_bus_emit_durable_event("test.provider.audit", "startup", "ok", "{}");
   test_calls_overlap();
   test_expired_deadline_does_not_block();

   atomic_store(&durable_ready, 1);
   obs_bus_flush();
   assert(atomic_load(&durable_writes) == 1);
   for (int i = 0; i < 8; i++)
      obs_bus_emit_durable_event("test.provider.audit", "shutdown", "ok", "{}");
   obs_bus_stop();
   obs_bus_module_service_stop(&service);
   assert(!service);
   assert(atomic_load(&durable_writes) == 9);
   assert(obs_bus_dropped() == 0);
   printf("ok\n");
   return 0;
}
