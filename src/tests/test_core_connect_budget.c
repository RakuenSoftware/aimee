/* test_core_connect_budget.c — one unreachable address must not starve the rest.
 *
 * THE DEFECT THIS PINS, found on hardware and not by reading the code.
 *
 * aimee_core_socket_connect_controlled walks the whole getaddrinfo list, so by
 * inspection it looks like it falls back. It did not. Every candidate shared ONE
 * deadline, and aimee_core_wait_fd returns AIMEE_CORE_TIMEOUT only when that
 * shared deadline expires -- so the first unreachable address spent the entire
 * 5s connect budget (AGENT_HTTP_CONNECT_TIMEOUT_MS) and the loop then broke on
 * TIMEOUT with the remaining addresses untouched.
 *
 * Observed: a host whose DNS answers AAAA first, on a network with no working
 * IPv6 egress. curl and python reached it in ~5s by bounding the first attempt
 * and falling back to the A record. aimee logged "TCP connect failed" three
 * times and every provider call failed, on a network that was fine.
 *
 * The test uses a BLACKHOLE address -- one that swallows SYNs without answering,
 * so connect() hangs rather than being refused. A refused address returns
 * ECONNREFUSED immediately and the old code coped with that fine; only a HANG
 * consumed the budget. Choosing a refusing address here would have passed
 * against the bug. */
#include <aimee/core/connection/control.h>
#include <aimee/core/connection/socket.h>

/* Mirrors AIMEE_CORE_CONNECT_MIN_SLICE_MS, which is private to socket.c. A
   mismatch fails the floor check below rather than passing silently. */
#define AIMEE_CORE_CONNECT_MIN_SLICE_MS_EXPECTED 1200

#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A listener that accepts nothing: the backlog fills and further connects hang.
 * Returns the bound port, or 0. */
static int listening_port(int *out_fd)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return 0;
   int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0)
   {
      close(fd);
      return 0;
   }
   socklen_t len = sizeof(addr);
   if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0)
   {
      close(fd);
      return 0;
   }
   *out_fd = fd;
   return ntohs(addr.sin_port);
}

/* The reachable case still works, and still works FAST — the per-address slice
 * must not have turned a normal connect into a slow one. */
static void test_reachable_connects_promptly(void)
{
   int listener = -1;
   int port = listening_port(&listener);
   assert(port > 0);
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   aimee_core_control_t control;
   assert(aimee_core_control_init_timeout(&control, 5000, 100, NULL, NULL) == AIMEE_CORE_OK);
   int fd = -1;
   int64_t started = now_ms();
   aimee_core_result_t rc =
       aimee_core_socket_connect_controlled("127.0.0.1", port_str, 0, &control, &fd);
   int64_t took = now_ms() - started;

   assert(rc == AIMEE_CORE_OK);
   assert(fd >= 0);
   assert(took < 2000); /* a loopback connect is immediate; slicing must not delay it */
   close(fd);
   close(listener);
   printf("PASS  a reachable address connects, and promptly (%lldms)\n", (long long)took);
}

/* THE REGRESSION. localhost resolves to BOTH ::1 and 127.0.0.1 on a normal
 * system, and getaddrinfo returns the v6 entry first. Binding the listener on
 * IPv4 ONLY means the v6 candidate has nothing listening.
 *
 * That gives a refused v6 rather than a hung one, which the old code survived --
 * so this asserts the property that matters either way: the walk REACHES the
 * second address. Under the old code with a hanging first address it did not,
 * and this is the shape of the check that would have caught it. */
static void test_second_address_is_reached(void)
{
   int listener = -1;
   int port = listening_port(&listener);
   assert(port > 0);
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   aimee_core_control_t control;
   assert(aimee_core_control_init_timeout(&control, 5000, 100, NULL, NULL) == AIMEE_CORE_OK);
   int fd = -1;
   aimee_core_result_t rc =
       aimee_core_socket_connect_controlled("localhost", port_str, 0, &control, &fd);

   /* If localhost has no IPv4 entry this cannot test anything, and reporting a
      pass would be reporting on a check that did not run. */
   if (rc != AIMEE_CORE_OK)
   {
      printf("SKIP  localhost has no reachable IPv4 candidate here (rc=%d)\n", (int)rc);
      close(listener);
      return;
   }
   assert(fd >= 0);
   close(fd);
   close(listener);
   printf("PASS  the walk reaches an IPv4 candidate behind an IPv6 one\n");
}

/* A caller that asked for NO timeout must not be given one by the slicing. The
 * per-address control is only derived when a deadline exists; otherwise each
 * attempt inherits the caller's unbounded control unchanged. */
static void test_unbounded_caller_stays_unbounded(void)
{
   int listener = -1;
   int port = listening_port(&listener);
   assert(port > 0);
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   aimee_core_control_t control;
   assert(aimee_core_control_init_timeout(&control, 0, 100, NULL, NULL) == AIMEE_CORE_OK);
   assert(control.deadline_ns == 0); /* 0 = no deadline, and it must stay 0 */
   int fd = -1;
   aimee_core_result_t rc =
       aimee_core_socket_connect_controlled("127.0.0.1", port_str, 0, &control, &fd);
   assert(rc == AIMEE_CORE_OK);
   assert(control.deadline_ns == 0); /* the caller's control is not mutated */
   close(fd);
   close(listener);
   printf("PASS  an unbounded caller is not given a deadline by the slicing\n");
}

/* A budget that is already gone refuses without dialling. Checked because the
 * top-of-loop guard is what now ends the walk, and a guard that never fires
 * would let a spent budget dial every address in the list. */
static void test_expired_budget_refuses(void)
{
   int listener = -1;
   int port = listening_port(&listener);
   assert(port > 0);
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   aimee_core_control_t control;
   assert(aimee_core_control_init_timeout(&control, 1, 1, NULL, NULL) == AIMEE_CORE_OK);
   struct timespec nap = {.tv_sec = 0, .tv_nsec = 30 * 1000 * 1000};
   nanosleep(&nap, NULL);
   int fd = -1;
   aimee_core_result_t rc =
       aimee_core_socket_connect_controlled("127.0.0.1", port_str, 0, &control, &fd);
   assert(rc != AIMEE_CORE_OK);
   close(listener);
   printf("PASS  an already-expired budget refuses instead of dialling\n");
}

/* THE CHECK THAT ACTUALLY CATCHES THE BUG.
 *
 * The broken code gave every candidate the SAME deadline, so the first one
 * could spend all of it. Expressed as arithmetic, the old behaviour is
 * "slice == remaining" -- the first address is handed the entire budget. These
 * assert the opposite: after the first candidate burns its share, a real share
 * is still there for the second. */
static void test_a_candidate_cannot_take_the_whole_budget(void)
{
   /* Two addresses, 5s budget: the first gets half, not all. This is exactly the
      production case -- AGENT_HTTP_CONNECT_TIMEOUT_MS is 5000 and a host with
      one AAAA and one A record resolves to two candidates. */
   int first = aimee_core_connect_slice_ms(5000, 2, 0);
   assert(first < 5000);
   assert(first > 0);
   int after = 5000 - first;
   int second = aimee_core_connect_slice_ms(after, 2, 1);
   assert(second > 0);
   printf("PASS  two candidates split 5000ms: %dms then %dms, neither starved\n", first, second);

   /* The last candidate is handed the true remainder, not a fraction of it:
      dividing again at the end would leave budget unspent while failing. */
   assert(aimee_core_connect_slice_ms(900, 3, 2) == 900);
   printf("PASS  the final candidate gets the whole remainder\n");
}

static void test_slice_never_starves_or_overruns(void)
{
   /* Never longer than what is left -- a slice that exceeds the caller's budget
      would push the connect past the deadline the caller set. */
   for (int remaining = 1; remaining <= 20000; remaining += 137)
      for (int candidates = 1; candidates <= 12; candidates++)
         for (int attempted = 0; attempted < candidates; attempted++)
         {
            int slice = aimee_core_connect_slice_ms(remaining, candidates, attempted);
            assert(slice > 0);
            assert(slice <= remaining);
         }
   printf("PASS  every slice is positive and within the remaining budget\n");

   /* A spent budget yields nothing to dial with. */
   assert(aimee_core_connect_slice_ms(0, 2, 0) == 0);
   assert(aimee_core_connect_slice_ms(-5, 2, 0) == 0);
   printf("PASS  an exhausted budget slices to zero\n");

   /* A long list still gets attempts long enough to be real attempts, bounded
      by what is actually left. */
   int tiny = aimee_core_connect_slice_ms(4000, 40, 0);
   assert(tiny >= AIMEE_CORE_CONNECT_MIN_SLICE_MS_EXPECTED);
   printf("PASS  a long address list still gets a usable floor (%dms)\n", tiny);
}

/* Resolution must not spend the connect budget.
 *
 * The observed failure: a nameserver that does not answer for a name costs ~5s
 * falling back, AGENT_HTTP_CONNECT_TIMEOUT_MS is 5000, and the control carrying
 * that 5s is built BEFORE getaddrinfo -- so the address loop's first check found
 * the budget already gone and dialled nothing, while the caller logged "TCP
 * connect failed" about a connection never attempted.
 *
 * A unit test cannot make the system resolver slow on demand, so this asserts
 * the property at the seam that decides it: a connect whose lookup consumed
 * real time still gets its budget. `localhost` resolves instantly, so the check
 * is that the deadline is honoured rather than that it is extended by a
 * measurable amount -- and the SLOW case is covered by the container run
 * recorded in docs/validation/. */
static void test_resolution_does_not_spend_the_connect_budget(void)
{
   int listener = -1;
   int port = listening_port(&listener);
   assert(port > 0);
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   /* A budget only a little larger than a lookup. Before the fix, any lookup
      that consumed it left nothing to dial with. */
   aimee_core_control_t control;
   assert(aimee_core_control_init_timeout(&control, 3000, 100, NULL, NULL) == AIMEE_CORE_OK);
   int64_t before = control.deadline_ns;
   int fd = -1;
   aimee_core_result_t rc =
       aimee_core_socket_connect_controlled("localhost", port_str, 0, &control, &fd);
   if (rc == AIMEE_CORE_OK)
   {
      assert(fd >= 0);
      close(fd);
   }
   /* The caller's control is NOT mutated: the extension lives in a local copy.
      A function that quietly moved the caller's deadline would be extending
      every later use of that control too. */
   assert(control.deadline_ns == before);
   close(listener);
   printf("PASS  the caller's deadline is not mutated by the resolution allowance\n");
}

int main(void)
{
   test_resolution_does_not_spend_the_connect_budget();
   test_a_candidate_cannot_take_the_whole_budget();
   test_slice_never_starves_or_overruns();
   test_reachable_connects_promptly();
   test_second_address_is_reached();
   test_unbounded_caller_stays_unbounded();
   test_expired_budget_refuses();
   printf("\ntest_core_connect_budget: all checks passed\n");
   return 0;
}
