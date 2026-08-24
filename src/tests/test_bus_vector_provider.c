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
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/db2/vector_contract.h>
#include <aimee/db2/vector_route.h>

#include <assert.h>
#include <stdatomic.h>
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

static int provider_serving(void)
{
   return obs_bus_module_available(AIMEE_VECTOR_EVENT_SEARCH);
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
           "publish = %u\n"
           "serve = %u\n",
           PROVIDER_CLASS, PROVIDER_REF, exe, AIMEE_VECTOR_EVENT_CAPABILITIES,
           AIMEE_VECTOR_EVENT_SEARCH);
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

/* What the provider was actually asked, recorded so the test can assert on it.
 *
 * The point of driving a real search rather than calling the transport directly:
 * a request that arrives with the wrong record type, dimension, top-k, scope or
 * kind filter is a provider answering a DIFFERENT question, and the reply to it
 * is well-formed, plausible and wrong. Nothing downstream would notice. */
static struct
{
   _Atomic uint64_t invocations;
   char record_type[AIMEE_VECTOR_MAX_RECORD_TYPE];
   char workspace[AIMEE_VECTOR_MAX_SCOPE];
   char project[AIMEE_VECTOR_MAX_SCOPE];
   uint32_t dimension;
   uint32_t top_k;
   uint64_t required_generation;
   uint32_t filter_count;
   char filter_key[AIMEE_VECTOR_MAX_LABEL_KEY];
   char filter_value[AIMEE_VECTOR_MAX_LABEL_VALUE];
   /* Set to answer the NEXT search with a reply naming a request that was never
    * made -- a provider answering out of band. */
   _Atomic int corrupt_request_id;
} g_served;

/* The provider's serve loop, on the provider's OWN attachment.
 *
 * Not aimee_module_process_run(), which attaches for itself -- and the host
 * admits exactly ONE live attachment per principal. A real provider is one
 * process holding one attachment that both publishes CAPABILITIES and serves
 * SEARCH, so a harness that needed a second attachment would be testing a
 * deployment shape that cannot exist.
 *
 * The first version used the runtime and ran it alongside the announcing
 * client. The announcing attachment silently became the server for SEARCH --
 * one grant carries both rights -- answered nothing, and every search died on
 * the deadline while the runtime was denied admission as a live duplicate. The
 * denial was correct; the harness was wrong.
 */
static int answer_invoke(bus_client_t *client, const bus_event_t *event)
{
   aimee_module_message_t invoke;
   if (aimee_module_message_decode(event->payload, event->payload_len, &invoke) !=
           AIMEE_MODULE_MESSAGE_OK ||
       invoke.operation != AIMEE_MODULE_OP_INVOKE)
      return 0;

   uint8_t body[AIMEE_MODULE_MESSAGE_HEADER_LEN + AIMEE_VECTOR_SEARCH_REPLY_HEADER +
                AIMEE_VECTOR_CANDIDATE_BYTES * AIMEE_VECTOR_MAX_TOP_K];
   uint16_t status = AIMEE_MODULE_STATUS_OK;
   size_t reply_len = 0;

   const uint8_t *request_body = event->payload + AIMEE_MODULE_MESSAGE_HEADER_LEN;
   aimee_vector_search_request_t request;
   float vector[AIMEE_VECTOR_MAX_DIM];
   aimee_vector_filter_view_t filters;
   if (invoke.stage_id != AIMEE_VECTOR_STAGE_SEARCH ||
       aimee_vector_search_request_decode(request_body, invoke.body_len, &request, vector,
                                          sizeof(vector) / sizeof(vector[0]), &filters) != 0)
      status = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   else
   {
      snprintf(g_served.record_type, sizeof(g_served.record_type), "%s", request.record_type);
      snprintf(g_served.workspace, sizeof(g_served.workspace), "%s", request.workspace);
      snprintf(g_served.project, sizeof(g_served.project), "%s", request.project);
      g_served.dimension = request.dimension;
      g_served.top_k = request.top_k;
      g_served.required_generation = request.required_generation;
      g_served.filter_count = (uint32_t)request.filter_count;
      g_served.filter_key[0] = '\0';
      g_served.filter_value[0] = '\0';
      aimee_vector_filter_entry_t entry;
      if (aimee_vector_filter_next(&filters, &entry) == 1)
      {
         snprintf(g_served.filter_key, sizeof(g_served.filter_key), "%.*s", (int)entry.key_length,
                  entry.key);
         const char *value = NULL;
         size_t value_length = 0;
         if (aimee_vector_filter_value(&filters, &entry, 0, &value, &value_length) == 0)
            snprintf(g_served.filter_value, sizeof(g_served.filter_value), "%.*s",
                     (int)value_length, value);
      }

      /* No candidates. This test opens no database, and every external candidate
       * is put through the deployment's own visibility check, which is a query --
       * covered by unit-test-vector-route-pgvec against live PostgreSQL. What is
       * proved here is the round trip: the request this process built reached a
       * provider intact, and the provider's answer came back and was accepted. */
      aimee_vector_search_reply_t reply;
      memset(&reply, 0, sizeof(reply));
      reply.request_id = atomic_load(&g_served.corrupt_request_id) ? request.request_id + 1000u
                                                                   : request.request_id;
      reply.generation = request.required_generation;
      reply.count = 0;
      if (aimee_vector_search_reply_encode(&reply, body + AIMEE_MODULE_MESSAGE_HEADER_LEN,
                                           sizeof(body) - AIMEE_MODULE_MESSAGE_HEADER_LEN,
                                           &reply_len) != 0)
         status = AIMEE_MODULE_STATUS_INTERNAL;
   }
   if (status != AIMEE_MODULE_STATUS_OK)
      reply_len = 0;

   aimee_module_message_t result = {.operation = AIMEE_MODULE_OP_RESULT,
                                    .status = status,
                                    .stage_id = invoke.stage_id ? invoke.stage_id : 1u,
                                    .body_len = (uint32_t)reply_len,
                                    .trace_id = invoke.trace_id};
   if (aimee_module_message_encode(&result, body, AIMEE_MODULE_MESSAGE_HEADER_LEN + reply_len) == 0)
      return 0;
   if (bus_client_reply(client, event->frame.event_kind, event->frame.correlation_id, body,
                        (uint32_t)(AIMEE_MODULE_MESSAGE_HEADER_LEN + reply_len)) != BUS_CLIENT_OK)
      return 0;
   atomic_fetch_add(&g_served.invocations, 1);
   return 1;
}

static _Atomic int g_serving_stop;

static void *run_serve_loop(void *arg)
{
   bus_client_t *client = arg;
   while (!atomic_load(&g_serving_stop))
   {
      bus_event_t event;
      if (bus_client_poll(client, &event) == BUS_CLIENT_OK)
      {
         if (event.frame.event_kind == AIMEE_VECTOR_EVENT_SEARCH)
            (void)answer_invoke(client, &event);
         continue;
      }
      tick();
   }
   return NULL;
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

   /* ---------------------------------------------------------------- searches
    *
    * Everything above proves a provider can be SELECTED. None of it proves a
    * search reaches one, and until this the answer was that none did: the route
    * had no transport, so a selected provider changed no query's answer.
    *
    * One attachment does both halves, because that is the only shape a provider
    * can have: one grant carries publish=CAPABILITIES and serve=SEARCH, and the
    * host admits one live attachment per principal. The attachment that has been
    * announcing is already the server for SEARCH; all that is missing is
    * somebody answering. */
   assert(kb_vector_provider_install_transport() == 0);
   /* Once, like the observer. */
   assert(kb_vector_provider_install_transport() == -1);

   /* Serving starts only now, on the attachment that has been announcing all
    * along -- the same principal, the same grant, one attachment. */
   assert(provider_serving() && "the grant that lets this principal announce also lets it serve");
   pthread_t provider_thread;
   assert(pthread_create(&provider_thread, NULL, run_serve_loop, &provider) == 0);

   /* A search with a tenancy statement, a kind restriction, and a provider ready
    * to serve it: the case the whole protocol exists for. */
   float vector[8];
   for (int i = 0; i < 8; ++i)
      vector[i] = (float)i / 8.0f;
   const char *kinds[] = {"decision"};
   int64_t ids[4];
   double scores[4];

   pgvec_memory_vector_scope_hint_set("acme", "widgets");
   uint64_t before = pgvec_memory_vector_provider_searches();
   int found = pgvec_memory_vector_search_with_kinds(vector, 8, kinds, 1, 4, ids, scores, 4);
   assert(found == 0 && "the provider answered with no candidates, which is 0 results");
   assert(pgvec_memory_vector_provider_searches() == before + 1);
   assert(atomic_load(&g_served.invocations) == 1);

   /* The provider was asked THIS search. A reply to a different question is
    * well-formed, plausible and wrong, and nothing downstream would notice. */
   assert(strcmp(g_served.record_type, "memory") == 0);
   assert(strcmp(g_served.workspace, "acme") == 0);
   assert(strcmp(g_served.project, "widgets") == 0);
   assert(g_served.dimension == 8);
   assert(g_served.top_k == 4);
   assert(g_served.required_generation == 1);
   assert(g_served.filter_count == 1);
   assert(strcmp(g_served.filter_key, "kind") == 0);
   assert(strcmp(g_served.filter_value, "decision") == 0);
   printf("  search reached the provider intact: %s/%s/%s dim=%u top_k=%u %s in [%s]\n",
          g_served.record_type, g_served.workspace, g_served.project, g_served.dimension,
          g_served.top_k, g_served.filter_key, g_served.filter_value);

   /* A search with no tenancy statement must not leave the deployment, even with
    * a provider selected and reachable. The counter is what proves it did not:
    * "returned no rows" is what a provider answering would also look like. */
   pgvec_memory_vector_scope_hint_clear();
   before = pgvec_memory_vector_provider_searches();
   uint64_t invocations = atomic_load(&g_served.invocations);
   (void)pgvec_memory_vector_search_record_type("memory", vector, 8, 4, ids, scores, 4);
   assert(pgvec_memory_vector_provider_searches() == before);
   assert(atomic_load(&g_served.invocations) == invocations);
   printf("  unscoped search never left the process\n");

   /* A provider answering a request nobody made. Fallback is disabled, so this
    * FAILS rather than quietly returning pgvector's answer to a query the
    * deployment's vectors are not in. */
   pgvec_memory_vector_scope_hint_set("acme", "widgets");
   atomic_store(&g_served.corrupt_request_id, 1);
   before = pgvec_memory_vector_provider_searches();
   found = pgvec_memory_vector_search_record_type("memory", vector, 8, 4, ids, scores, 4);
   assert(found < 0 && "a reply naming the wrong request was accepted");
   /* Counted as answered, because it was: the transport got a readable reply,
    * and the route is what refused it. */
   assert(pgvec_memory_vector_provider_searches() == before + 1);
   atomic_store(&g_served.corrupt_request_id, 0);
   pgvec_memory_vector_scope_hint_clear();
   printf("  reply naming the wrong request refused, and not silently downgraded\n");

   atomic_store(&g_serving_stop, 1);
   pthread_join(provider_thread, NULL);
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
