/* test_db2_pool.c: bounded DB2 connection pool — lease/return/timeout/
 * concurrency/poison/reaper. Uses the db2_pool test seam (mock connection ops)
 * so no Postgres is required. */
#include "db2/db2_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Link stubs: db2_pool's default ops (g_open/g_close + member_reset_real)
 * reference these libpq wrappers. The test injects mocks, so these are never
 * called — they only satisfy the linker without pulling in libpq. */
void *aimee_pg_open(const char *u, char *e, size_t n)
{
   (void)u;
   (void)e;
   (void)n;
   return NULL;
}
void aimee_pg_close(void *c)
{
   (void)c;
}
int aimee_pg_in_transaction(void *c)
{
   (void)c;
   return 0; /* member_reset_real is shimmed (g_reset) in these tests */
}
int aimee_pg_exec(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   return -1;
}

/* --- mock connection ops ------------------------------------------------- */

typedef struct
{
   int id;
   int in_use; /* set while a thread "holds" it — double-lease detector */
} mock_conn_t;

static int g_open_count = 0;
static int g_close_count = 0;
static int g_reset_count = 0;
static int g_reset_rc = 0; /* set to -1 to force a poison */
static pthread_mutex_t g_mock_mtx = PTHREAD_MUTEX_INITIALIZER;

static void *mock_open(const char *url, char *e, size_t n)
{
   (void)url;
   (void)e;
   (void)n;
   mock_conn_t *c = calloc(1, sizeof(*c));
   pthread_mutex_lock(&g_mock_mtx);
   c->id = ++g_open_count;
   pthread_mutex_unlock(&g_mock_mtx);
   return c;
}
static void mock_close(void *p)
{
   pthread_mutex_lock(&g_mock_mtx);
   g_close_count++;
   pthread_mutex_unlock(&g_mock_mtx);
   free(p);
}
static int mock_reset(void *p)
{
   (void)p;
   pthread_mutex_lock(&g_mock_mtx);
   g_reset_count++;
   int rc = g_reset_rc;
   pthread_mutex_unlock(&g_mock_mtx);
   return rc;
}

static void install_mock(void)
{
   db2_pool_set_test_ops(mock_open, mock_close, mock_reset);
   g_open_count = g_close_count = g_reset_count = 0;
   g_reset_rc = 0;
}

/* --- tests --------------------------------------------------------------- */

static void test_lease_return_basic(void)
{
   install_mock();
   char e[128] = "";
   assert(db2_pool_init("mock://x", 2, e, sizeof(e)) == 0);
   assert(db2_pool_active());
   assert(g_open_count == 0); /* lazy: nothing opened until first lease */

   void *a = db2_pool_lease(1000);
   void *b = db2_pool_lease(1000);
   assert(a && b && a != b);  /* two distinct connections */
   assert(g_open_count == 2); /* opened on demand */

   db2_pool_return(a);
   assert(g_reset_count >= 1); /* return resets */
   void *c = db2_pool_lease(1000);
   assert(c == a); /* freed member is handed back */
   db2_pool_return(b);
   db2_pool_return(c);
   db2_pool_shutdown();
   assert(!db2_pool_active());
   assert(g_close_count == 2); /* both members closed on shutdown */
}

static void test_exhaustion_timeout(void)
{
   install_mock();
   char e[128] = "";
   assert(db2_pool_init("mock://x", 1, e, sizeof(e)) == 0);
   void *a = db2_pool_lease(1000);
   assert(a);
   /* pool empty -> short lease must time out, not hang */
   void *b = db2_pool_lease(150);
   assert(b == NULL);
   long timeouts = 0;
   db2_pool_stats(NULL, NULL, NULL, NULL, &timeouts, NULL, NULL);
   assert(timeouts >= 1);
   db2_pool_return(a);
   void *c = db2_pool_lease(1000); /* now available */
   assert(c == a);
   db2_pool_return(c);
   db2_pool_shutdown();
}

/* Concurrency: N threads hammer a pool of M < N; assert no connection is ever
 * handed to two threads at once (the in_use flag) and nothing deadlocks. */
#define NTHREADS 16
#define POOLM    4
#define ITERS    200
static volatile int g_conc_fail = 0;

static void *worker(void *arg)
{
   (void)arg;
   for (int i = 0; i < ITERS; i++)
   {
      mock_conn_t *c = (mock_conn_t *)db2_pool_lease(5000);
      if (!c)
      {
         g_conc_fail = 1;
         return NULL;
      }
      /* exclusive ownership check */
      if (__sync_lock_test_and_set(&c->in_use, 1) != 0)
         g_conc_fail = 1;
      /* tiny critical section */
      for (volatile int k = 0; k < 50; k++)
      {
      }
      __sync_lock_release(&c->in_use);
      db2_pool_return(c);
   }
   return NULL;
}

static void test_concurrency_no_double_lease(void)
{
   install_mock();
   char e[128] = "";
   assert(db2_pool_init("mock://x", POOLM, e, sizeof(e)) == 0);
   pthread_t th[NTHREADS];
   for (int i = 0; i < NTHREADS; i++)
      assert(pthread_create(&th[i], NULL, worker, NULL) == 0);
   for (int i = 0; i < NTHREADS; i++)
      pthread_join(th[i], NULL);
   assert(g_conc_fail == 0); /* no double-lease, no timeout-starvation */
   long grants = 0;
   db2_pool_stats(NULL, NULL, NULL, &grants, NULL, NULL, NULL);
   assert(grants == (long)NTHREADS * ITERS);
   db2_pool_shutdown();
}

static void test_poison_on_reset_fail(void)
{
   install_mock();
   char e[128] = "";
   assert(db2_pool_init("mock://x", 1, e, sizeof(e)) == 0);
   assert(g_open_count == 0); /* lazy */
   void *a = db2_pool_lease(1000);
   assert(a);
   assert(g_open_count == 1); /* opened on demand */
   g_reset_rc = -1;           /* next reset fails -> poison */
   db2_pool_return(a);        /* closes + reopens the member */
   g_reset_rc = 0;
   assert(g_close_count == 1); /* poisoned member closed */
   assert(g_open_count == 2);  /* and a fresh one opened */
   long poisoned = 0;
   db2_pool_stats(NULL, NULL, NULL, NULL, NULL, NULL, &poisoned);
   assert(poisoned == 1);
   void *b = db2_pool_lease(1000); /* pool still usable */
   assert(b);
   db2_pool_return(b);
   db2_pool_shutdown();
}

/* Reaper observability: a lease held past the ceiling (a missed lease_end) is
 * detected + counted (NOT reclaimed — reclaim-on-death is the WP-B destructor). */
static void test_reaper_detects_stuck_lease(void)
{
   install_mock();
   char e[128] = "";
   assert(db2_pool_init("mock://x", 2, e, sizeof(e)) == 0);
   db2_pool_set_test_ceiling_ms(50);
   void *a = db2_pool_lease(1000); /* hold it (simulate missed lease_end) */
   assert(a);
   assert(db2_pool_reaper_sweep() == 0); /* not yet past ceiling */
   usleep(120 * 1000);                   /* exceed the 50ms ceiling */
   assert(db2_pool_reaper_sweep() == 1); /* stuck lease flagged */
   long stuck = 0;
   db2_pool_stats(NULL, NULL, NULL, NULL, NULL, &stuck, NULL);
   assert(stuck >= 1);
   db2_pool_return(a); /* the connection is NOT corrupted; still usable */
   void *b = db2_pool_lease(1000);
   assert(b);
   db2_pool_return(b);
   db2_pool_shutdown();
}

int main(void)
{
   test_lease_return_basic();
   test_exhaustion_timeout();
   test_concurrency_no_double_lease();
   test_poison_on_reset_fail();
   test_reaper_detects_stuck_lease();
   printf("db2_pool: all tests passed\n");
   return 0;
}
