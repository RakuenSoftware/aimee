/* test_openai_runs_store.c — unit tests for the live /v1/runs store.
 *
 * Covers the proposal's runs-store acceptance criteria at the data-structure
 * level (the HTTP/worker wiring is exercised by the server integration tests):
 *   - create / status / get and duplicate rejection;
 *   - queued -> in_progress -> terminal status transitions and snapshot updates;
 *   - status string + terminal helpers;
 *   - append-only event buffering and ordered drain via openai_runs_store_wait;
 *   - cancellation flag semantics;
 *   - LIVE cross-thread delivery (a producer thread's events wake a blocked
 *     subscriber before the run completes — not a post-hoc replay).
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "openai_runs_store.h"

/* ---- 1. create / status / get / duplicate ---- */
static void test_create_status_get(void)
{
   openai_runs_store_reset();
   const char *snap = "{\"id\":\"run_1\",\"object\":\"run\",\"status\":\"queued\"}";
   assert(openai_runs_store_create("run_1", snap) == 1);
   /* duplicate id is rejected */
   assert(openai_runs_store_create("run_1", snap) == 0);
   /* empty args rejected */
   assert(openai_runs_store_create("", snap) == 0);
   assert(openai_runs_store_create("run_2", "") == 0);

   openai_run_status_t st;
   assert(openai_runs_store_status("run_1", &st) == 1);
   assert(st == OPENAI_RUN_QUEUED);

   char buf[256];
   assert(openai_runs_store_get("run_1", buf, sizeof(buf)) == 1);
   assert(strcmp(buf, snap) == 0);

   /* unknown id */
   assert(openai_runs_store_status("nope", &st) == 0);
   assert(openai_runs_store_get("nope", buf, sizeof(buf)) == 0);
   assert(buf[0] == '\0');
   printf("  create_status_get: ok\n");
}

/* ---- 2. status transitions + snapshot update + terminal lockout ---- */
static void test_transitions(void)
{
   openai_runs_store_reset();
   assert(openai_runs_store_create("run_t", "{\"status\":\"queued\"}") == 1);

   openai_runs_store_set_status("run_t", OPENAI_RUN_IN_PROGRESS);
   openai_run_status_t st;
   assert(openai_runs_store_status("run_t", &st) == 1 && st == OPENAI_RUN_IN_PROGRESS);

   openai_runs_store_update_json("run_t", "{\"status\":\"in_progress\"}");
   char buf[128];
   assert(openai_runs_store_get("run_t", buf, sizeof(buf)) == 1);
   assert(strcmp(buf, "{\"status\":\"in_progress\"}") == 0);

   openai_runs_store_finalize("run_t", OPENAI_RUN_COMPLETED, "{\"status\":\"completed\"}");
   assert(openai_runs_store_status("run_t", &st) == 1 && st == OPENAI_RUN_COMPLETED);
   assert(openai_runs_store_get("run_t", buf, sizeof(buf)) == 1);
   assert(strcmp(buf, "{\"status\":\"completed\"}") == 0);

   /* once terminal, set_status and append_event are no-ops */
   openai_runs_store_set_status("run_t", OPENAI_RUN_IN_PROGRESS);
   assert(openai_runs_store_status("run_t", &st) == 1 && st == OPENAI_RUN_COMPLETED);
   openai_runs_store_append_event("run_t", "late", "{}");
   size_t cursor = 0;
   char ev[64], data[64];
   /* no events buffered, run terminal -> TERMINAL immediately */
   assert(openai_runs_store_wait("run_t", &cursor, 0, ev, sizeof(ev), data, sizeof(data)) ==
          OPENAI_RUNS_WAIT_TERMINAL);
   printf("  transitions: ok\n");
}

/* ---- 3. status string + terminal helpers ---- */
static void test_status_helpers(void)
{
   assert(strcmp(openai_run_status_str(OPENAI_RUN_QUEUED), "queued") == 0);
   assert(strcmp(openai_run_status_str(OPENAI_RUN_IN_PROGRESS), "in_progress") == 0);
   assert(strcmp(openai_run_status_str(OPENAI_RUN_COMPLETED), "completed") == 0);
   assert(strcmp(openai_run_status_str(OPENAI_RUN_CANCELLED), "cancelled") == 0);
   assert(strcmp(openai_run_status_str(OPENAI_RUN_FAILED), "failed") == 0);

   assert(openai_run_status_terminal(OPENAI_RUN_QUEUED) == 0);
   assert(openai_run_status_terminal(OPENAI_RUN_IN_PROGRESS) == 0);
   assert(openai_run_status_terminal(OPENAI_RUN_COMPLETED));
   assert(openai_run_status_terminal(OPENAI_RUN_CANCELLED));
   assert(openai_run_status_terminal(OPENAI_RUN_FAILED));
   printf("  status_helpers: ok\n");
}

/* ---- 4. generation-aware missing-run classification ---- */
static void test_missing_classification(void)
{
   const char *generation = openai_runs_store_generation();
   assert(generation && generation[0] == 'g');
   char current[160];
   snprintf(current, sizeof(current), "oprun_%s_1700000000_1", generation);
   assert(openai_runs_store_classify_missing(current) == OPENAI_RUNS_MISSING_EVICTED);
   assert(openai_runs_store_classify_missing("oprun_gprior_1700000000_1") ==
          OPENAI_RUNS_MISSING_INTERRUPTED);
   assert(openai_runs_store_classify_missing("oprun_1700000000_1") ==
          OPENAI_RUNS_MISSING_INTERRUPTED);
   assert(openai_runs_store_classify_missing("run_unknown") == OPENAI_RUNS_MISSING_UNKNOWN);
   assert(openai_runs_store_classify_missing("oprun_malformed") == OPENAI_RUNS_MISSING_UNKNOWN);
   printf("  missing_classification: ok\n");
}

/* ---- 5. event buffering + ordered drain (single thread) ---- */
static void test_event_buffer(void)
{
   openai_runs_store_reset();
   assert(openai_runs_store_create("run_e", "{}") == 1);
   openai_runs_store_append_event("run_e", "a", "{\"i\":1}");
   openai_runs_store_append_event("run_e", "b", "{\"i\":2}");

   size_t cursor = 0;
   char ev[64], data[64];
   assert(openai_runs_store_wait("run_e", &cursor, 50, ev, sizeof(ev), data, sizeof(data)) ==
          OPENAI_RUNS_WAIT_EVENT);
   assert(strcmp(ev, "a") == 0 && strcmp(data, "{\"i\":1}") == 0);
   assert(openai_runs_store_wait("run_e", &cursor, 50, ev, sizeof(ev), data, sizeof(data)) ==
          OPENAI_RUNS_WAIT_EVENT);
   assert(strcmp(ev, "b") == 0 && strcmp(data, "{\"i\":2}") == 0);
   /* nothing more, not terminal -> TIMEOUT */
   assert(openai_runs_store_wait("run_e", &cursor, 50, ev, sizeof(ev), data, sizeof(data)) ==
          OPENAI_RUNS_WAIT_TIMEOUT);

   openai_runs_store_finalize("run_e", OPENAI_RUN_COMPLETED, "{}");
   assert(openai_runs_store_wait("run_e", &cursor, 50, ev, sizeof(ev), data, sizeof(data)) ==
          OPENAI_RUNS_WAIT_TERMINAL);

   /* wait on an unknown run -> GONE */
   size_t c2 = 0;
   assert(openai_runs_store_wait("ghost", &c2, 0, ev, sizeof(ev), data, sizeof(data)) ==
          OPENAI_RUNS_WAIT_GONE);
   printf("  event_buffer: ok\n");
}

/* ---- 6. cancellation ---- */
static void test_cancel(void)
{
   openai_runs_store_reset();
   assert(openai_runs_store_create("run_c", "{}") == 1);
   assert(openai_runs_store_cancel_requested("run_c") == 0);
   assert(openai_runs_store_request_cancel("run_c") == 1);
   assert(openai_runs_store_cancel_requested("run_c") == 1);
   /* once terminal, request_cancel returns 0 */
   openai_runs_store_finalize("run_c", OPENAI_RUN_CANCELLED, "{}");
   assert(openai_runs_store_request_cancel("run_c") == 0);
   /* unknown id */
   assert(openai_runs_store_request_cancel("nope") == 0);
   printf("  cancel: ok\n");
}

/* ---- 7. live cross-thread delivery ---- */
static void *producer(void *arg)
{
   (void)arg;
   usleep(20000); /* 20ms: subscriber should be blocked in wait() by now */
   openai_runs_store_append_event("run_live", "tick", "{}");
   usleep(20000);
   openai_runs_store_finalize("run_live", OPENAI_RUN_COMPLETED, "{\"object\":\"run\"}");
   return NULL;
}

static void test_live_stream(void)
{
   openai_runs_store_reset();
   assert(openai_runs_store_create("run_live", "{}") == 1);

   pthread_t th;
   assert(pthread_create(&th, NULL, producer, NULL) == 0);

   size_t cursor = 0;
   char ev[64], data[64];
   int saw_tick = 0, saw_terminal = 0;
   for (int i = 0; i < 100; i++) /* bounded so a bug can't hang the suite */
   {
      openai_runs_wait_t w =
          openai_runs_store_wait("run_live", &cursor, 500, ev, sizeof(ev), data, sizeof(data));
      if (w == OPENAI_RUNS_WAIT_EVENT)
      {
         if (strcmp(ev, "tick") == 0)
            saw_tick = 1;
      }
      else if (w == OPENAI_RUNS_WAIT_TERMINAL)
      {
         saw_terminal = 1;
         break;
      }
      else if (w == OPENAI_RUNS_WAIT_GONE)
      {
         break;
      }
      /* TIMEOUT: loop again */
   }
   pthread_join(th, NULL);
   assert(saw_tick == 1);     /* the producer's live append woke the subscriber */
   assert(saw_terminal == 1); /* terminal observed after the event, not before */
   printf("  live_stream: ok\n");
}

int main(void)
{
   printf("openai_runs_store:\n");
   test_create_status_get();
   test_transitions();
   test_status_helpers();
   test_missing_classification();
   test_event_buffer();
   test_cancel();
   test_live_stream();
   openai_runs_store_reset();
   printf("All openai_runs_store tests passed.\n");
   return 0;
}
