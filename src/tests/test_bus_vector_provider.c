/* test_bus_vector_provider.c: a CAPABILITIES announcement from an
 * authenticated provider must move the memory route's selection.
 *
 * WHY THIS ATTACHES OVER THE MODULE SOCKET rather than publishing on the
 * in-process producer. The first version of this test did the easy thing and
 * failed for the right reason: the in-process producer attaches anonymously, so
 * its frames carry principal_ref 0, and the registry refuses principal 0 --
 * correctly, because a provider that cannot be identified cannot be selected.
 *
 * That failure is the whole argument for this file's shape. A provider is an
 * external process that connects to the module runtime socket and is
 * authenticated by uid and executable against a .grant file, and only then does
 * the host stamp its principal onto every frame it publishes. Any cheaper
 * publisher tests a path production does not have.
 *
 * So this test writes a grant naming its own executable, starts obs_bus with a
 * module runtime, connects to that socket as a provider, and announces. The
 * assertion is on `pgvec_memory_vector_selected_provider()` -- the value a
 * deployment acts on -- and on the exact principal from the grant, so a
 * selection that moved for some other reason cannot satisfy it.
 *
 * The negative cases carry as much weight as the positive one. A provider that
 * is not ready must not be selected; a frame this build cannot decode must be
 * counted rather than silently dropped, because otherwise "no provider selected"
 * would mean both "none attached" and "one is announcing in a dialect we do not
 * speak", which are different problems with different fixes.
 *
 * Opens no database: the route is used for its selection state only. */
#define _GNU_SOURCE

#include "kb/db2_adapters/kb_vector_provider.h"
#include "modules/db2/c/memory_vectors.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/db2/vector_contract.h>
#include <aimee/db2/vector_route.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The provider this test pretends to be. Any value the grant and the host agree
 * on; asserted exactly, so a selection that moved for another reason fails. */
#define PROVIDER_CLASS 1u
#define PROVIDER_REF   4242u

static char g_dir[PATH_MAX];
static char g_socket[PATH_MAX];
static char g_policy[PATH_MAX];

static void tick(void)
{
   const struct timespec t = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
   nanosleep(&t, NULL);
}

/* Poll rather than sleep a fixed time: a fixed sleep is either flaky or slow,
 * and on a loaded machine it is both. */
static int wait_for(int (*done)(void))
{
   for (int i = 0; i < 1500; ++i)
   {
      if (done())
         return 1;
      tick();
   }
   return 0;
}

static uint64_t seen_target;
static int seen_reached(void)
{
   return pgvec_memory_vector_capabilities_seen() >= seen_target;
}

static uint64_t rejected_target;
static int rejected_reached(void)
{
   return pgvec_memory_vector_capabilities_rejected() >= rejected_target;
}

static int provider_selected(void)
{
   return pgvec_memory_vector_selected_provider() == PROVIDER_REF;
}

static void write_grant(void)
{
   char exe[PATH_MAX];
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   assert(n > 0);
   exe[n] = '\0';

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/provider.grant", g_policy);
   FILE *f = fopen(path, "w");
   assert(f);
   /* uid=self and this executable: the runtime authenticates the connecting
    * peer's credentials against these before it will admit it at all. */
   fprintf(f,
           "version = 1\n"
           "principal_class = %u\n"
           "principal_ref = %u\n"
           "uid = self\n"
           "executable = %s\n"
           "publish = %u\n",
           PROVIDER_CLASS, PROVIDER_REF, exe, AIMEE_VECTOR_EVENT_CAPABILITIES);
   assert(fclose(f) == 0);
}

static int connect_as_provider(bus_client_t *client)
{
   int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_socket);
   /* The listener starts with obs_bus; a connect immediately after start can
    * still lose the race with the accept thread's first bind. */
   for (int i = 0; i < 500; ++i)
   {
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      {
         if (bus_client_attach_as(fd, client, PROVIDER_CLASS, PROVIDER_REF) == BUS_CLIENT_OK)
            return fd;
         close(fd);
         return -1;
      }
      tick();
   }
   close(fd);
   return -1;
}

static void announce(bus_client_t *client, const aimee_vector_capabilities_t *capabilities)
{
   uint8_t frame[AIMEE_VECTOR_CAPABILITIES_HEADER];
   size_t written = 0;
   assert(aimee_vector_capabilities_encode(capabilities, frame, sizeof(frame), &written) == 0);
   assert(bus_client_publish(client, AIMEE_VECTOR_EVENT_CAPABILITIES, frame, (uint32_t)written) ==
          BUS_CLIENT_OK);
}

static aimee_vector_capabilities_t eligible(uint64_t generation)
{
   aimee_vector_capabilities_t capabilities = {
       .generation = generation,
       .operations = AIMEE_VECTOR_OPERATION_SEARCH,
       .metrics = AIMEE_VECTOR_METRIC_COSINE,
       .filters = AIMEE_VECTOR_FILTER_EXACT,
       .max_dimension = 1024,
       .max_top_k = 128,
       .ready = 1,
   };
   return capabilities;
}

/* One full run, differing only in WHEN the host registers its observer.
 *
 * Both orderings must work, and they are separate code paths: a registration
 * made before obs_bus_start() is subscribed by the sweep inside start, and one
 * made after subscribes itself. Production takes the FIRST of those --
 * kb_service_init() runs before obs_bus_start() in kb_main -- so testing only
 * the convenient one would leave the shipped path uncovered.
 *
 * Each ordering needs its own process because obs_bus is process-global and
 * does not restart, so main() forks. The temp directory is named by pid and is
 * therefore already distinct per run. */
static int run(int register_before_start)
{
   setvbuf(stdout, NULL, _IOLBF, 0);

   /* TMPDIR, not /tmp: the unit-test runner exports a per-run TMPDIR and removes
    * it on exit, and a hardcoded /tmp path leaks one directory per run forever.
    * The suite has been to the end of that road already -- tmpfs out of inodes
    * with 45GB free. */
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !*tmp)
      tmp = "/tmp";
   snprintf(g_dir, sizeof(g_dir), "%s/aimee-vector-observe-%d", tmp, (int)getpid());
   snprintf(g_socket, sizeof(g_socket), "%s/bus.sock", g_dir);
   snprintf(g_policy, sizeof(g_policy), "%s/policy", g_dir);
   assert(mkdir(g_dir, 0700) == 0);
   assert(mkdir(g_policy, 0700) == 0);
   write_grant();

   if (obs_bus_configure_module_runtime(g_socket, g_policy) != 0)
   {
      fprintf(stderr, "bus_vector_provider: module runtime config failed\n");
      return 1;
   }

   /* The KB's own subscription, called exactly as kb_service_init calls it: db2
    * subscribes to nothing, the host delivers to it. */
   if (register_before_start)
      assert(kb_vector_provider_observe() == 0);

   if (obs_bus_start() != 0)
   {
      fprintf(stderr, "bus_vector_provider: obs_bus_start failed\n");
      return 1;
   }

   if (!register_before_start)
      assert(kb_vector_provider_observe() == 0);
   assert(pgvec_memory_vector_selected_provider() == 0);
   /* Two observers of one kind is an ownership question, not a fan-out feature.
    * Asserted after start in both orderings, so the post-start entry path is
    * entered even in the run that registered early. */
   assert(kb_vector_provider_observe() == -1);

   bus_client_t provider;
   if (connect_as_provider(&provider) < 0)
   {
      fprintf(stderr, "bus_vector_provider: provider could not attach\n");
      return 1;
   }
   printf("  provider attached as principal %u\n", PROVIDER_REF);

   /* Not ready: announced, delivered, and NOT selected. The `seen` counter is
    * what makes this case worth anything -- without it, "not selected" would
    * also be the answer if nothing had been delivered at all, which is exactly
    * how the first version of this test passed while proving nothing. */
   aimee_vector_capabilities_t not_ready = eligible(0);
   not_ready.ready = 0;
   seen_target = pgvec_memory_vector_capabilities_seen() + 1;
   announce(&provider, &not_ready);
   assert(wait_for(seen_reached) && "the announcement never reached the observer");
   assert(pgvec_memory_vector_selected_provider() == 0);
   printf("  not-ready provider delivered (seen=%llu), still on pgvector\n",
          (unsigned long long)pgvec_memory_vector_capabilities_seen());

   /* Ready: selected, and selected as THIS principal -- taken from the frame the
    * host stamped, not from anything the payload said. */
   aimee_vector_capabilities_t ready = eligible(9);
   announce(&provider, &ready);
   assert(wait_for(provider_selected) && "a ready provider did not become selected");
   printf("  ready provider selected: principal=%u\n", pgvec_memory_vector_selected_provider());

   /* A later announcement from the same attachment, higher sequence: accepted,
    * selection unchanged. */
   seen_target = pgvec_memory_vector_capabilities_seen() + 1;
   aimee_vector_capabilities_t again = eligible(10);
   announce(&provider, &again);
   assert(wait_for(seen_reached));
   assert(pgvec_memory_vector_selected_provider() == PROVIDER_REF);

   /* A frame this build cannot read is counted, not ignored -- and does not
    * disturb a working selection. */
   rejected_target = pgvec_memory_vector_capabilities_rejected() + 1;
   uint8_t malformed[AIMEE_VECTOR_CAPABILITIES_HEADER];
   memset(malformed, 0, sizeof(malformed));
   assert(bus_client_publish(&provider, AIMEE_VECTOR_EVENT_CAPABILITIES, malformed,
                             (uint32_t)sizeof(malformed)) == BUS_CLIENT_OK);
   assert(wait_for(rejected_reached) && "an unreadable announcement was silently dropped");
   assert(pgvec_memory_vector_selected_provider() == PROVIDER_REF);
   printf("  malformed announcement counted (rejected=%llu), selection unchanged\n",
          (unsigned long long)pgvec_memory_vector_capabilities_rejected());

   bus_client_detach(&provider);
   obs_bus_stop();

   /* Leave nothing behind even when run outside the suite's TMPDIR sandbox. */
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/provider.grant", g_policy);
   unlink(path);
   unlink(g_socket);
   rmdir(g_policy);
   rmdir(g_dir);
   printf("  ok: observer registered %s obs_bus_start\n",
          register_before_start ? "before" : "after");
   return 0;
}

int main(void)
{
   /* The child takes the production ordering. If fork fails the run is not
    * silently reduced to half its coverage -- it fails. */
   pid_t child = fork();
   if (child < 0)
   {
      perror("bus_vector_provider: fork");
      return 1;
   }
   if (child == 0)
      _exit(run(1));

   int rc = run(0);

   int status = 0;
   if (waitpid(child, &status, 0) != child)
   {
      perror("bus_vector_provider: waitpid");
      return 1;
   }
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
   {
      fprintf(stderr, "bus_vector_provider: pre-start registration run failed\n");
      return 1;
   }
   if (rc != 0)
      return rc;
   printf("test_bus_vector_provider: OK\n");
   return 0;
}
