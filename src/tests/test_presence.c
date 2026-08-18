/* test_presence.c: unit tests for the unified-presence registry (presence.c).
 *
 * Covers the contract in src/headers/presence.h: attachment registry + owner
 * reconciliation, turn arbitration (acquire/queue/busy/release/fairness),
 * workspace single-writer leases, the presence-event ring (publish/wait/
 * heartbeat/eviction), and outbound routing through an injected delivery stub.
 *
 * Self-contained: no test framework. main() runs the checks, prints a line per
 * assertion failure, and returns the failure count (0 == all pass). Links
 * against presence.o + delivery_target.o only (see tests/Rules.mk). */
#include "presence.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         g_fail++;                                                                                 \
         printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                    \
      }                                                                                            \
   } while (0)

#define CHECK_EQ_INT(a, b)                                                                         \
   do                                                                                              \
   {                                                                                               \
      long _va = (long)(a), _vb = (long)(b);                                                       \
      if (_va != _vb)                                                                              \
      {                                                                                            \
         g_fail++;                                                                                 \
         printf("FAIL %s:%d  %s (%ld) == %s (%ld)\n", __FILE__, __LINE__, #a, _va, #b, _vb);       \
      }                                                                                            \
   } while (0)

/* ---- delivery stub (header typedef: int(*)(const delivery_target_t*,
 * const char*, void*)) --------------------------------------------------- */
static int g_stub_calls;
static char g_stub_last_text[256];
static char g_stub_last_platform[32];
static void *g_stub_last_user;

static int delivery_stub(const delivery_target_t *target, const char *text, void *user)
{
   g_stub_calls++;
   g_stub_last_user = user;
   snprintf(g_stub_last_text, sizeof(g_stub_last_text), "%s", text ? text : "");
   snprintf(g_stub_last_platform, sizeof(g_stub_last_platform), "%s",
            target ? target->platform : "");
   return 0;
}

static void reset_stub(void)
{
   g_stub_calls = 0;
   g_stub_last_text[0] = '\0';
   g_stub_last_platform[0] = '\0';
   g_stub_last_user = NULL;
}

/* ---- tests ------------------------------------------------------------- */

static void test_attach_owner_and_count(void)
{
   const char *sid = "sess-attach";
   char a1[64] = "", a2[64] = "";

   CHECK_EQ_INT(presence_attachment_count(sid), 0);

   /* First attach creates the presence and fixes the owner. */
   CHECK_EQ_INT(presence_attach(sid, "uid:1000", "cli", NULL, PRESENCE_EV_ALL, 0, a1, sizeof(a1)),
                1);
   CHECK(a1[0] != '\0');
   CHECK_EQ_INT(presence_attachment_count(sid), 1);

   /* A second attach with the same owner succeeds; count reflects it. */
   CHECK_EQ_INT(
       presence_attach(sid, "uid:1000", "webchat", NULL, PRESENCE_EV_TURN, 0, a2, sizeof(a2)), 1);
   CHECK(a2[0] != '\0');
   CHECK(strcmp(a1, a2) != 0); /* distinct attach ids */
   CHECK_EQ_INT(presence_attachment_count(sid), 2);

   /* A third attach with a MISMATCHED non-empty owner is refused. */
   char a3[64] = "";
   CHECK_EQ_INT(
       presence_attach(sid, "uid:2000", "telegram", NULL, PRESENCE_EV_ALL, 0, a3, sizeof(a3)), 0);
   CHECK_EQ_INT(presence_attachment_count(sid), 2);

   /* Missing surface is rejected. */
   CHECK_EQ_INT(presence_attach(sid, "uid:1000", "", NULL, 0, 0, a3, sizeof(a3)), 0);

   /* Detaching both non-persistent attachments tears the presence down. */
   CHECK_EQ_INT(presence_detach(sid, a1), 1);
   CHECK_EQ_INT(presence_attachment_count(sid), 1);
   CHECK_EQ_INT(presence_detach(sid, a2), 1);
   CHECK_EQ_INT(presence_attachment_count(sid), 0);

   /* Now gone: detaching a stale id is a no-op. */
   CHECK_EQ_INT(presence_detach(sid, a1), 0);
}

static void test_persistent_keeps_alive(void)
{
   const char *sid = "sess-persist";
   char a1[64] = "";
   /* Persistent attach with a real delivery target. */
   CHECK_EQ_INT(presence_attach(sid, "uid:1000", "telegram", "telegram:-100:7",
                                PRESENCE_EV_DELEGATE, 1, a1, sizeof(a1)),
                1);
   CHECK_EQ_INT(presence_attachment_count(sid), 1);

   /* Detaching the live stream leaves a routing-only persistent slot: the
    * presence survives (still listed) but has zero live attachments. */
   CHECK_EQ_INT(presence_detach(sid, a1), 1);
   CHECK_EQ_INT(presence_attachment_count(sid), 0);

   char js[512];
   CHECK_EQ_INT(presence_session_json(sid, js, sizeof(js)), 1); /* still exists */
   CHECK(strstr(js, sid) != NULL);
}

/* The wedge, and the way out of it.
 *
 * A turn is released by whoever acquired it. When that party stops existing
 * nothing releases it, and turn_in_flight is a bare flag — so the session
 * declined every later submit with presence_busy permanently, and the only
 * cure was restarting the server. These pin the age signal that makes the
 * abandonment visible, and the reclaim that undoes it. */
static void test_turn_reclaim(void)
{
   const char *sid = "sess-reclaim";
   char aA[64] = "", aB[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, aA, sizeof(aA)), 1);
   CHECK_EQ_INT(presence_attach(sid, "u", "webchat", NULL, PRESENCE_EV_ALL, 0, aB, sizeof(aB)), 1);

   /* No turn in flight reads as an absence, not as age zero: a caller must be
    * able to tell "nothing is running" from "something started this second". */
   CHECK_EQ_INT((int)presence_turn_inflight_age(sid, NULL, 0), -1);

   char turn[64] = "", stuck[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, aA, 0, turn, sizeof(turn), NULL, 0, NULL),
                PRESENCE_TURN_ACQUIRED);

   /* In flight: a non-negative age, and the live turn id reported with it, so
    * the caller reclaims the turn it measured rather than "whatever is running
    * now" — which by then may be someone else's healthy turn. */
   CHECK(presence_turn_inflight_age(sid, stuck, sizeof(stuck)) >= 0);
   CHECK(strcmp(stuck, turn) == 0);

   /* A stale id reclaims nothing. This is the race guard: between deciding a
    * turn was abandoned and acting on it, the real holder may have finished
    * and a new turn begun. */
   CHECK_EQ_INT(presence_turn_reclaim(sid, "turn-does-not-exist"), 0);
   CHECK_EQ_INT(presence_turn_is_live(sid, turn), 1);
   CHECK_EQ_INT(presence_turn_reclaim(sid, ""), 0);
   CHECK_EQ_INT(presence_turn_reclaim(sid, NULL), 0);
   CHECK_EQ_INT(presence_turn_is_live(sid, turn), 1);

   /* B is stuck behind it — the reported symptom. */
   char infl[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, aB, 0, NULL, 0, infl, sizeof(infl), NULL),
                PRESENCE_TURN_BUSY);
   CHECK(strcmp(infl, turn) == 0);

   /* Reclaiming it frees the session, and B — who never held it — can run. */
   CHECK_EQ_INT(presence_turn_reclaim(sid, turn), 1);
   CHECK_EQ_INT(presence_turn_is_live(sid, turn), 0);
   CHECK_EQ_INT((int)presence_turn_inflight_age(sid, NULL, 0), -1);

   char turnB[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, aB, 0, turnB, sizeof(turnB), NULL, 0, NULL),
                PRESENCE_TURN_ACQUIRED);
   CHECK(strcmp(turnB, turn) != 0);

   /* Reclaiming twice does nothing the second time: two callers may both decide
    * the same turn is abandoned, and the loser must not go on to kill the
    * winner's fresh turn. */
   CHECK_EQ_INT(presence_turn_reclaim(sid, turn), 0);
   CHECK_EQ_INT(presence_turn_is_live(sid, turnB), 1);

   CHECK_EQ_INT(presence_turn_release(sid, turnB), 1);
   presence_detach(sid, aA);
   presence_detach(sid, aB);
}

static void test_turn_arbitration(void)
{
   const char *sid = "sess-turn";
   char aA[64] = "", aB[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, aA, sizeof(aA)), 1);
   CHECK_EQ_INT(presence_attach(sid, "u", "webchat", NULL, PRESENCE_EV_ALL, 0, aB, sizeof(aB)), 1);

   /* A acquires the turn. */
   char turn[64] = "", infl[64] = "";
   int pos = -1;
   presence_turn_result_t r =
       presence_turn_acquire(sid, aA, 0, turn, sizeof(turn), infl, sizeof(infl), &pos);
   CHECK_EQ_INT(r, PRESENCE_TURN_ACQUIRED);
   CHECK(turn[0] != '\0');
   CHECK_EQ_INT(presence_turn_is_live(sid, turn), 1);

   /* B (no queue) gets BUSY with A's live turn id. */
   infl[0] = '\0';
   r = presence_turn_acquire(sid, aB, 0, NULL, 0, infl, sizeof(infl), NULL);
   CHECK_EQ_INT(r, PRESENCE_TURN_BUSY);
   CHECK(strcmp(infl, turn) == 0);

   /* B (want_queue) gets QUEUED at position 1. */
   pos = -1;
   r = presence_turn_acquire(sid, aB, 1, NULL, 0, NULL, 0, &pos);
   CHECK_EQ_INT(r, PRESENCE_TURN_QUEUED);
   CHECK_EQ_INT(pos, 1);

   /* Re-queueing B is idempotent (still position 1). */
   pos = -1;
   r = presence_turn_acquire(sid, aB, 1, NULL, 0, NULL, 0, &pos);
   CHECK_EQ_INT(r, PRESENCE_TURN_QUEUED);
   CHECK_EQ_INT(pos, 1);

   /* Releasing the wrong turn id is a no-op; the right one releases. */
   CHECK_EQ_INT(presence_turn_release(sid, "turn-does-not-exist"), 0);
   CHECK_EQ_INT(presence_turn_release(sid, turn), 1);
   CHECK_EQ_INT(presence_turn_is_live(sid, turn), 0);

   /* Now B, as the queue head, can acquire (fairness: it was first in line). */
   char turnB[64] = "";
   r = presence_turn_acquire(sid, aB, 0, turnB, sizeof(turnB), NULL, 0, NULL);
   CHECK_EQ_INT(r, PRESENCE_TURN_ACQUIRED);
   CHECK(turnB[0] != '\0');
   CHECK(strcmp(turnB, turn) != 0);
   CHECK_EQ_INT(presence_turn_release(sid, turnB), 1);

   presence_detach(sid, aA);
   presence_detach(sid, aB);
}

static void test_workspace_leases(void)
{
   const char *sid = "sess-lease";
   char aA[64] = "", aB[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, aA, sizeof(aA)), 1);
   CHECK_EQ_INT(presence_attach(sid, "u", "webchat", NULL, PRESENCE_EV_ALL, 0, aB, sizeof(aB)), 1);

   const char *ws = "/repo/main";
   CHECK_EQ_INT(presence_workspace_lease_acquire(sid, ws, aA), 1); /* granted */
   CHECK_EQ_INT(presence_workspace_lease_acquire(sid, ws, aB), 0); /* denied: A holds it */
   CHECK_EQ_INT(presence_workspace_lease_acquire(sid, ws, aA), 1); /* reentrant for A */

   char holder[64] = "";
   CHECK_EQ_INT(presence_workspace_lease_holder(sid, ws, holder, sizeof(holder)), 1);
   CHECK(strcmp(holder, aA) == 0);

   /* B cannot release A's lease; A can. Then B can acquire. */
   CHECK_EQ_INT(presence_workspace_lease_release(sid, ws, aB), 0);
   CHECK_EQ_INT(presence_workspace_lease_release(sid, ws, aA), 1);
   CHECK_EQ_INT(presence_workspace_lease_holder(sid, ws, holder, sizeof(holder)), 0);
   CHECK_EQ_INT(presence_workspace_lease_acquire(sid, ws, aB), 1);

   /* Detaching the lease holder releases the lease automatically. */
   presence_detach(sid, aB);
   CHECK_EQ_INT(presence_workspace_lease_holder(sid, ws, holder, sizeof(holder)), 0);
   presence_detach(sid, aA);
}

static void test_event_ring(void)
{
   const char *sid = "sess-events";
   char a1[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, a1, sizeof(a1)), 1);

   uint64_t cursor = 0;
   char ev[64], data[256];

   /* Publish then read it back. */
   presence_publish(sid, PRESENCE_EV_REFLECT, "hello", "{\"k\":1}");
   presence_wait_t w = presence_wait(sid, &cursor, 100, ev, sizeof(ev), data, sizeof(data));
   CHECK_EQ_INT(w, PRESENCE_WAIT_EVENT);
   CHECK(strcmp(ev, "hello") == 0);
   CHECK(strstr(data, "\"k\":1") != NULL);

   /* No further events: a bounded wait times out. */
   w = presence_wait(sid, &cursor, 50, ev, sizeof(ev), data, sizeof(data));
   CHECK_EQ_INT(w, PRESENCE_WAIT_TIMEOUT);

   /* A turn acquisition publishes an observable turn_started event. */
   char turn[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, a1, 0, turn, sizeof(turn), NULL, 0, NULL),
                PRESENCE_TURN_ACQUIRED);
   w = presence_wait(sid, &cursor, 100, ev, sizeof(ev), data, sizeof(data));
   CHECK_EQ_INT(w, PRESENCE_WAIT_EVENT);
   CHECK(strcmp(ev, "turn_started") == 0);
   CHECK(strstr(data, turn) != NULL);
   presence_turn_release(sid, turn);

   /* Waiting on an unknown session reports GONE. */
   uint64_t c2 = 0;
   w = presence_wait("no-such-session", &c2, 10, ev, sizeof(ev), data, sizeof(data));
   CHECK_EQ_INT(w, PRESENCE_WAIT_GONE);

   presence_detach(sid, a1);
}

static void test_outbound_routing(void)
{
   const char *sid = "sess-route";
   char aPhone[64] = "", aCli[64] = "";

   /* A persistent telegram target subscribed to DELEGATE events. */
   CHECK_EQ_INT(presence_attach(sid, "u", "telegram", "telegram:-100:7", PRESENCE_EV_DELEGATE, 1,
                                aPhone, sizeof(aPhone)),
                1);
   /* A CLI stream subscribed only to TURN events, no delivery target. */
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_TURN, 0, aCli, sizeof(aCli)), 1);

   int marker = 42;
   presence_set_delivery_fn(delivery_stub, &marker);
   reset_stub();

   /* A DELEGATE event routes to the phone (subscribed) but not the CLI. */
   int n = presence_route_event(sid, PRESENCE_EV_DELEGATE, "delegate_done", "PR is green");
   CHECK_EQ_INT(n, 1);
   CHECK_EQ_INT(g_stub_calls, 1);
   CHECK(strcmp(g_stub_last_text, "PR is green") == 0);
   CHECK(strcmp(g_stub_last_platform, "telegram") == 0);
   CHECK(g_stub_last_user == &marker);

   /* The routed event is also observable on the live SSE ring. */
   uint64_t cursor = 0;
   char ev[64], data[256];
   presence_wait_t w = presence_wait(sid, &cursor, 100, ev, sizeof(ev), data, sizeof(data));
   CHECK_EQ_INT(w, PRESENCE_WAIT_EVENT);
   CHECK(strcmp(ev, "delegate_done") == 0);
   CHECK(strstr(data, "PR is green") != NULL);

   /* A TURN-class route reaches no delivery target (phone isn't subscribed to
    * TURN, CLI has no target). */
   reset_stub();
   n = presence_route_event(sid, PRESENCE_EV_TURN, "turn_done", "done");
   CHECK_EQ_INT(n, 0);
   CHECK_EQ_INT(g_stub_calls, 0);

   /* Persistent target still receives after its live stream detaches. */
   reset_stub();
   CHECK_EQ_INT(presence_detach(sid, aPhone), 1); /* routing-only now */
   n = presence_route_event(sid, PRESENCE_EV_DELEGATE, "delegate_done", "still you");
   CHECK_EQ_INT(n, 1);
   CHECK_EQ_INT(g_stub_calls, 1);
   CHECK(strcmp(g_stub_last_text, "still you") == 0);

   /* With no delivery callback, routing dispatches nothing but still publishes
    * to the ring. */
   presence_set_delivery_fn(NULL, NULL);
   reset_stub();
   uint64_t c2 = 0;
   /* drain existing events first */
   while (presence_wait(sid, &c2, 0, ev, sizeof(ev), data, sizeof(data)) == PRESENCE_WAIT_EVENT)
      ;
   n = presence_route_event(sid, PRESENCE_EV_DELEGATE, "delegate_done", "no fn");
   CHECK_EQ_INT(n, 0);
   CHECK_EQ_INT(g_stub_calls, 0);
   w = presence_wait(sid, &c2, 100, ev, sizeof(ev), data, sizeof(data));
   CHECK_EQ_INT(w, PRESENCE_WAIT_EVENT);
   CHECK(strstr(data, "no fn") != NULL);

   presence_detach(sid, aCli);
}

static void test_single_attachment_regression(void)
{
   /* One surface, one turn cycle — behaves exactly like today's single
    * connection: acquire, run, release, with no busy/queue surprises. */
   const char *sid = "sess-solo";
   char a[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, a, sizeof(a)), 1);
   char turn[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, a, 0, turn, sizeof(turn), NULL, 0, NULL),
                PRESENCE_TURN_ACQUIRED);
   CHECK_EQ_INT(presence_turn_is_live(sid, turn), 1);
   CHECK_EQ_INT(presence_turn_release(sid, turn), 1);
   /* The same surface can immediately take another turn. */
   char turn2[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, a, 0, turn2, sizeof(turn2), NULL, 0, NULL),
                PRESENCE_TURN_ACQUIRED);
   CHECK_EQ_INT(presence_turn_release(sid, turn2), 1);
   presence_detach(sid, a);
   CHECK_EQ_INT(presence_attachment_count(sid), 0);
}

static void test_session_close(void)
{
   /* Closing a session tears the presence down even with a persistent target
    * (which presence_detach would otherwise keep alive), and wakes event
    * waiters with GONE. */
   const char *sid = "sess-close";
   char a[64] = "";
   CHECK_EQ_INT(
       presence_attach(sid, "u", "telegram", "telegram:-1:2", PRESENCE_EV_ALL, 1, a, sizeof(a)), 1);
   char js[256];
   CHECK_EQ_INT(presence_session_json(sid, js, sizeof(js)), 1); /* exists */

   CHECK_EQ_INT(presence_session_close(sid), 1);
   CHECK_EQ_INT(presence_session_close(sid), 0); /* already gone */
   CHECK_EQ_INT(presence_attachment_count(sid), 0);
   CHECK_EQ_INT(presence_session_json(sid, js, sizeof(js)), 0); /* torn down */

   /* A waiter on a closed session reports GONE immediately. */
   uint64_t cursor = 0;
   char ev[32], data[64];
   CHECK_EQ_INT(presence_wait(sid, &cursor, 50, ev, sizeof(ev), data, sizeof(data)),
                PRESENCE_WAIT_GONE);
}

/* Releases a held turn after a short delay, to wake a blocked acquire_wait. */
struct delayed_release
{
   char session[64];
   char turn[64];
   int delay_us;
};
static void *delayed_release_fn(void *arg)
{
   struct delayed_release *r = (struct delayed_release *)arg;
   usleep(r->delay_us);
   presence_turn_release(r->session, r->turn);
   return NULL;
}

static void test_turn_queue_wait(void)
{
   const char *sid = "sess-qwait";
   char aA[64] = "", aB[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, aA, sizeof(aA)), 1);
   CHECK_EQ_INT(presence_attach(sid, "u", "tui", NULL, PRESENCE_EV_ALL, 0, aB, sizeof(aB)), 1);

   /* Free turn → acquire_wait returns immediately. */
   char turnA[64] = "";
   CHECK_EQ_INT(presence_turn_acquire_wait(sid, aA, 1000, turnA, sizeof(turnA), NULL, 0),
                PRESENCE_TURN_ACQUIRED);
   CHECK(turnA[0] != '\0');

   /* A holds it → B waits and times out (BUSY), leaving the queue clean. */
   char infl[64] = "";
   CHECK_EQ_INT(presence_turn_acquire_wait(sid, aB, 60, NULL, 0, infl, sizeof(infl)),
                PRESENCE_TURN_BUSY);
   CHECK(strcmp(infl, turnA) == 0);

   /* A releases from another thread while B blocks → B acquires (FIFO wake). */
   struct delayed_release r;
   snprintf(r.session, sizeof(r.session), "%s", sid);
   snprintf(r.turn, sizeof(r.turn), "%s", turnA);
   r.delay_us = 40 * 1000;
   pthread_t th;
   CHECK_EQ_INT(pthread_create(&th, NULL, delayed_release_fn, &r), 0);
   char turnB[64] = "";
   presence_turn_result_t rb =
       presence_turn_acquire_wait(sid, aB, 3000, turnB, sizeof(turnB), NULL, 0);
   pthread_join(th, NULL);
   CHECK_EQ_INT(rb, PRESENCE_TURN_ACQUIRED);
   CHECK(turnB[0] != '\0' && strcmp(turnB, turnA) != 0);
   CHECK_EQ_INT(presence_turn_is_live(sid, turnB), 1);
   presence_turn_release(sid, turnB);

   presence_detach(sid, aA);
   presence_detach(sid, aB);
}

/* Concurrent cross-surface delivery: a watcher thread loops presence_wait()
 * (exactly as GET /v1/sessions/{id}/events does on its connection thread) while
 * another thread runs a turn (acquire→release) — mirroring a turn driven by a
 * *different* surface. The watcher must observe turn_started and turn_done.
 * The other tests cover this sequentially; this guards the threaded path
 * (publish on one thread, presence_wait blocked in pthread_cond_timedwait on
 * another) that the live SSE event stream depends on. */
struct watch_ctx
{
   const char *sid;
   volatile int stop;
   int saw_started;
   int saw_done;
};
static void *cross_surface_watcher(void *arg)
{
   struct watch_ctx *w = (struct watch_ctx *)arg;
   uint64_t cursor = 0; /* fresh subscriber, like handle_session_events */
   char ev[64], data[256];
   while (!w->stop)
   {
      presence_wait_t r = presence_wait(w->sid, &cursor, 200, ev, sizeof(ev), data, sizeof(data));
      if (r == PRESENCE_WAIT_EVENT)
      {
         if (strcmp(ev, "turn_started") == 0)
            w->saw_started = 1;
         else if (strcmp(ev, "turn_done") == 0)
            w->saw_done = 1;
      }
      else if (r == PRESENCE_WAIT_GONE)
         break;
   }
   return NULL;
}
static void test_cross_surface_concurrent(void)
{
   const char *sid = "sess-xsurface";
   char aA[64] = "", aB[64] = "";
   CHECK_EQ_INT(presence_attach(sid, "u", "webchat", NULL, PRESENCE_EV_ALL, 0, aA, sizeof(aA)), 1);
   CHECK_EQ_INT(presence_attach(sid, "u", "cli", NULL, PRESENCE_EV_ALL, 0, aB, sizeof(aB)), 1);

   struct watch_ctx wc = {sid, 0, 0, 0};
   pthread_t th;
   pthread_create(&th, NULL, cross_surface_watcher, &wc);
   usleep(100 * 1000); /* let the watcher enter presence_wait */

   /* A turn driven by surface B (a different attachment than the watcher). */
   char turn[64] = "";
   CHECK_EQ_INT(presence_turn_acquire(sid, aB, 0, turn, sizeof(turn), NULL, 0, NULL),
                PRESENCE_TURN_ACQUIRED);
   usleep(150 * 1000);
   CHECK_EQ_INT(presence_turn_release(sid, turn), 1);
   usleep(150 * 1000);

   wc.stop = 1;
   pthread_join(th, NULL);
   CHECK(wc.saw_started); /* cross-surface turn_started delivered */
   CHECK(wc.saw_done);    /* cross-surface turn_done delivered    */

   presence_detach(sid, aA);
   presence_detach(sid, aB);
}

int main(void)
{
   presence_init();

   test_cross_surface_concurrent();
   test_attach_owner_and_count();
   test_persistent_keeps_alive();
   test_turn_arbitration();
   test_turn_reclaim();
   test_workspace_leases();
   test_event_ring();
   test_outbound_routing();
   test_single_attachment_regression();
   test_session_close();
   test_turn_queue_wait();

   presence_shutdown();

   if (g_fail == 0)
      printf("PASS test_presence (all checks)\n");
   else
      printf("FAIL test_presence: %d check(s) failed\n", g_fail);
   return g_fail;
}
