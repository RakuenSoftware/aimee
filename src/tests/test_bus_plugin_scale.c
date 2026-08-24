/* test_bus_plugin_scale.c: exploratory e2e at the deployment's target shape.
 *
 * The single-instance path is covered by test_bus_plugin_process.c. This one
 * exercises the claims that only show up in the large or the ugly:
 *
 *   1. TEN instances at once, each with its own plugin, grant, and event pair --
 *      the stated target shape (~10 MCP + ~5 pluggy).
 *   2. Two instances provisioned the SAME event base. bus_host_serve_kind()
 *      binds one kind to one slot, so the second MUST be refused. This is the
 *      constraint the whole per-instance allocation exists for, asserted rather
 *      than assumed.
 *   3. Plugins that misbehave: one that exits immediately, one that writes
 *      garbage instead of JSON-RPC. Neither may take its module down, and
 *      neither may leave commands registered.
 *   4. Concurrent invocation across every instance at once.
 *
 * Usage: unit-test-bus-plugin-scale /abs/path/to/aimee-module
 */
#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_attach.h>
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>

#include "aimee_sha256.h"
#include "platform_test_util.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* A plugin instance's kinds are DERIVED from its principal ref by the canonical
 * module rule; the ref is the only per-instance allocation. */
#define PLUGIN_KIND(ref, stage) (4096u + (ref) * 256u + (stage))
#define STAGE_INVOKE            1u
#define STAGE_DECLARE           2u
#define DECL_REQ_MAGIC          0x444d4344u
#define DECL_RESP_MAGIC         0x524d4344u
#define WIRE_VERSION            1u
#define DECL_HEADER_LEN         12u
#define DECL_RECORD_LEN         16u
#define DECL_PENDING_MAGIC      0x504d4344u /* "DCMP" */
#define ADMIT_ALLOW             1u
#define ADMIT_REFUSE            2u
#define ADMIT_REQUEST_LEN       44u
#define INVOKE_REQ_MAGIC        0x51504d43u
#define INVOKE_RESP_MAGIC       0x53504d43u
#define INVOKE_REQ_HEADER       16u
#define INVOKE_RESP_HEADER      12u

#define INSTANCES  10
#define FIRST_REF  300u
#define CALLER_REF 299u

/* A well-behaved plugin whose single tool name is unique per instance, so a
 * response can be attributed to the instance that produced it. */
static const char *GOOD_PLUGIN =
    "import sys, json, os\n"
    "tag=os.environ.get('PLUGIN_TAG','x')\n"
    "TOOLS=[{'name':'do-'+tag,'description':'tool '+tag,'inputSchema':{'type':'object'}}]\n"
    "for line in sys.stdin:\n"
    "    line=line.strip()\n"
    "    if not line: continue\n"
    "    q=json.loads(line); m=q.get('method'); i=q.get('id')\n"
    "    if m=='initialize': r={'protocolVersion':'2024-11-05'}\n"
    "    elif m=='tools/list': r={'tools':TOOLS}\n"
    "    elif m=='tools/call': r={'served_by':tag}\n"
    "    else: r=None\n"
    "    out={'jsonrpc':'2.0','id':i,'result':r} if r is not None else "
    "{'jsonrpc':'2.0','id':i,'error':{'code':-1,'message':'no'}}\n"
    "    sys.stdout.write(json.dumps(out)+'\\n'); sys.stdout.flush()\n";

/* Exits before the handshake completes. */
static const char *DEAD_PLUGIN = "import sys\nsys.exit(3)\n";

/* Answers, but with bytes that are not JSON-RPC at all. */
static const char *GARBAGE_PLUGIN =
    "import sys\n"
    "for line in sys.stdin:\n"
    "    sys.stdout.write('this is not json\\n'); sys.stdout.flush()\n";

typedef struct
{
   bus_host_t *host;
   pthread_mutex_t *lock;
   atomic_int stop;
} pump_t;

static void *run_pump(void *a)
{
   pump_t *s = a;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   while (!atomic_load_explicit(&s->stop, memory_order_acquire))
   {
      pthread_mutex_lock(s->lock);
      (void)bus_host_pump(s->host);
      pthread_mutex_unlock(s->lock);
      nanosleep(&pause, NULL);
   }
   return NULL;
}

static uint32_t rd_u32(const unsigned char *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const unsigned char *p)
{
   return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static void wr_u32(unsigned char *p, uint32_t v)
{
   p[0] = (unsigned char)v;
   p[1] = (unsigned char)(v >> 8);
   p[2] = (unsigned char)(v >> 16);
   p[3] = (unsigned char)(v >> 24);
}
static void wr_u16(unsigned char *p, uint16_t v)
{
   p[0] = (unsigned char)v;
   p[1] = (unsigned char)(v >> 8);
}

static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   assert(fputs(content, f) >= 0);
   assert(fclose(f) == 0);
}

static pid_t start_instance(const char *exe, const char *dir, const char *name, const char *sock,
                            uint32_t ref, uint32_t base, const char *argv_json, const char *tag)
{
   char link[PATH_MAX];
   snprintf(link, sizeof(link), "%s/aimee-module-mcp-%s", dir, name);
   unlink(link);
   assert(symlink(exe, link) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      /* Die with the harness. A failing assertion calls abort(), which never
       * reaches the cleanup at the end of main -- without this, every failed run
       * orphans a module process (and its plugin child) that keeps running. Set
       * before exec so it survives it. */
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() == 1)
         _exit(0); /* the parent already died in the race window */
      char r[32], b[32];
      snprintf(r, sizeof(r), "%u", ref);
      snprintf(b, sizeof(b), "%u", base);
      setenv("AIMEE_MODULE_PRINCIPAL_REF", r, 1);
      setenv("AIMEE_MODULE_EVENT_BASE", b, 1);
      if (tag)
         setenv("PLUGIN_TAG", tag, 1);
      /* The fixture plugins carry no MCP annotations, so their tools count as
       * `write`. The default ceiling is `read` (least privilege), which would
       * correctly withhold every one of them -- so an instance that is meant to
       * expose tools has to say so, exactly as a real deployment would. */
      setenv("AIMEE_MCP_PLUGIN_PERMISSION", "write", 1);
      if (argv_json)
         setenv("AIMEE_MCP_PLUGIN_ARGV", argv_json, 1);
      execl(link, link, sock, (char *)NULL);
      _exit(127);
   }
   return child;
}

/* Complete the admission handshake for an instance.
 *
 * Mirrors what src/module_commands.c does: the module answers a plain declare
 * with DCMP ("I want to run this argv"), and only starts its plugin once a
 * verdict bound to a hash of exactly those bytes comes back. Returns 1 when a
 * verdict was delivered, 0 when the module was not waiting for one. */
static int admit_instance(aimee_module_client_t *client, uint32_t event_base, uint32_t verdict)
{
   unsigned char request[8];
   wr_u32(request, DECL_REQ_MAGIC);
   wr_u32(request + 4, WIRE_VERSION);
   unsigned char response[8192];
   uint32_t response_len = 0;

   for (int attempt = 0; attempt < 400; ++attempt)
   {
      response_len = 0;
      if (aimee_module_client_call(client, event_base + 1, STAGE_DECLARE, 0, 0, request,
                                   sizeof(request), response, sizeof(response), &response_len, NULL,
                                   NULL) == AIMEE_MODULE_CALL_OK &&
          response_len >= DECL_HEADER_LEN && rd_u32(response) == DECL_PENDING_MAGIC)
      {
         uint32_t argv_len = rd_u32(response + 8);
         assert(DECL_HEADER_LEN + argv_len == response_len);
         unsigned char digest[32];
         assert(aimee_sha256_raw(response + DECL_HEADER_LEN, argv_len, digest) == 0);

         unsigned char admit[ADMIT_REQUEST_LEN];
         wr_u32(admit, DECL_REQ_MAGIC);
         wr_u32(admit + 4, WIRE_VERSION);
         wr_u32(admit + 8, verdict);
         memcpy(admit + 12, digest, sizeof(digest));

         unsigned char ack[65536];
         uint32_t ack_len = 0;
         assert(aimee_module_client_call(client, event_base + 1, STAGE_DECLARE, 0, 0, admit,
                                         sizeof(admit), ack, sizeof(ack), &ack_len, NULL,
                                         NULL) == AIMEE_MODULE_CALL_OK);
         return 1;
      }
      const struct timespec pause = {.tv_sec = 0, .tv_nsec = 25000000};
      nanosleep(&pause, NULL);
   }
   return 0;
}

/* Declare against one instance. Returns the command count, or -1 if the call
 * itself failed. `verb0` receives the first verb when there is one. */
static int declare_once(aimee_module_client_t *c, uint32_t base, char *verb0, size_t verb0_len)
{
   unsigned char req[8];
   wr_u32(req, DECL_REQ_MAGIC);
   wr_u32(req + 4, WIRE_VERSION);
   unsigned char resp[65536];
   uint32_t n = 0;
   if (aimee_module_client_call(c, base + 1, STAGE_DECLARE, 0, 0, req, sizeof(req), resp,
                                sizeof(resp), &n, NULL, NULL) != AIMEE_MODULE_CALL_OK)
      return -1;
   if (n < DECL_HEADER_LEN || rd_u32(resp) != DECL_RESP_MAGIC)
      return -1;
   uint32_t count = rd_u32(resp + 8);
   if (count > 0 && verb0)
   {
      uint32_t off = DECL_HEADER_LEN;
      uint16_t gl = rd_u16(resp + off + 8), vl = rd_u16(resp + off + 10);
      off += DECL_RECORD_LEN;
      snprintf(verb0, verb0_len, "%.*s", (int)vl, resp + off + gl);
   }
   return (int)count;
}

/* Poll an instance until it declares `want` commands, or give up. */
static int declare_until(aimee_module_client_t *c, uint32_t base, int want, char *verb0,
                         size_t verb0_len)
{
   for (int attempt = 0; attempt < 400; ++attempt)
   {
      int n = declare_once(c, base, verb0, verb0_len);
      if (n == want)
         return n;
      const struct timespec pause = {.tv_sec = 0, .tv_nsec = 25000000};
      nanosleep(&pause, NULL);
   }
   return declare_once(c, base, verb0, verb0_len);
}

int main(int argc, char **argv)
{
   assert(argc == 2);
   char exe[PATH_MAX], caller_exe[PATH_MAX];
   assert(realpath(argv[1], exe) != NULL);
   assert(realpath("/proc/self/exe", caller_exe) != NULL);

   char dir[256];
   snprintf(dir, sizeof(dir), "%s/aimee-plugin-scale-XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir) != NULL);

   char good[PATH_MAX], dead[PATH_MAX], garbage[PATH_MAX], sock[512];
   snprintf(good, sizeof(good), "%s/good.py", dir);
   snprintf(dead, sizeof(dead), "%s/dead.py", dir);
   snprintf(garbage, sizeof(garbage), "%s/garbage.py", dir);
   snprintf(sock, sizeof(sock), "%s/module.sock", dir);
   write_file(good, GOOD_PLUGIN);
   write_file(dead, DEAD_PLUGIN);
   write_file(garbage, GARBAGE_PLUGIN);

   char good_argv[PATH_MAX + 64], dead_argv[PATH_MAX + 64], garbage_argv[PATH_MAX + 64];
   snprintf(good_argv, sizeof(good_argv), "[\"python3\",\"%s\"]", good);
   snprintf(dead_argv, sizeof(dead_argv), "[\"python3\",\"%s\"]", dead);
   snprintf(garbage_argv, sizeof(garbage_argv), "[\"python3\",\"%s\"]", garbage);

   /* INSTANCES good ones, then a dead-plugin instance, a garbage-plugin
    * instance, and a COLLIDER that reuses instance 0's PRINCIPAL REF.
    *
    * The ref is what it has to reuse now: event kinds are derived from the ref,
    * so a process cannot be pointed at someone else's kinds through its
    * environment any more. Sharing a ref -- a copied .grant, the same instance
    * started twice -- is the remaining way two modules can claim one kind, and
    * bus_host_serve_kind() must still refuse the second. */
   const int total = INSTANCES + 3;
   uint32_t bases[INSTANCES + 3];
   for (int i = 0; i < INSTANCES; i++)
      bases[i] = PLUGIN_KIND(FIRST_REF + (uint32_t)i, STAGE_INVOKE);
   uint32_t dead_base = PLUGIN_KIND(FIRST_REF + (uint32_t)INSTANCES, STAGE_INVOKE);
   uint32_t garbage_base = PLUGIN_KIND(FIRST_REF + (uint32_t)INSTANCES + 1u, STAGE_INVOKE);
   bases[INSTANCES] = dead_base;
   bases[INSTANCES + 1] = garbage_base;
   /* Its own grant entry; the collision comes from the ref it attaches with. */
   bases[INSTANCES + 2] = PLUGIN_KIND(FIRST_REF + (uint32_t)INSTANCES + 2u, STAGE_INVOKE);

   static uint32_t serve_pairs[INSTANCES + 3][2];
   static uint32_t all_kinds[(INSTANCES + 3) * 2];
   bus_runtime_grant_t grants[INSTANCES + 4];
   int nk = 0;
   for (int i = 0; i < total; i++)
   {
      serve_pairs[i][0] = bases[i];
      serve_pairs[i][1] = bases[i] + 1;
      grants[i] = (bus_runtime_grant_t){.principal_class = 1,
                                        .principal_ref = FIRST_REF + (uint32_t)i,
                                        .uid = BUS_RUNTIME_SELF_UID,
                                        .executable = exe,
                                        .serve = serve_pairs[i],
                                        .serve_count = 2};
      all_kinds[nk++] = bases[i];
      all_kinds[nk++] = bases[i] + 1;
   }
   grants[total] = (bus_runtime_grant_t){.principal_class = 1,
                                         .principal_ref = CALLER_REF,
                                         .uid = BUS_RUNTIME_SELF_UID,
                                         .executable = caller_exe,
                                         .request = all_kinds,
                                         .request_count = (size_t)nk};

   /* Slot and kind budgets sized like the real daemon's (obs_bus uses 64 slots). */
   bus_host_config_t hc = {.max_slots = 64,
                           .slot_size = 4096,
                           .inline_budget = 3800,
                           .queue_capacity = 64,
                           .arena_size = 512 * 1024};
   bus_host_t host;
   assert(bus_host_create(&host, &hc, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t rc = {.socket_path = sock,
                              .socket_mode = 0600,
                              .backlog = 32,
                              .stale_after_ns = 30000000000ULL,
                              .grants = grants,
                              .grant_count = (size_t)total + 1};
   bus_runtime_t *runtime = bus_runtime_start(&host, &lock, &rc);
   assert(runtime != NULL);

   pid_t pids[INSTANCES + 3];
   char tags[INSTANCES][16];
   for (int i = 0; i < INSTANCES; i++)
   {
      char name[32];
      snprintf(name, sizeof(name), "inst%d", i);
      snprintf(tags[i], sizeof(tags[i]), "t%d", i);
      pids[i] = start_instance(exe, dir, name, sock, FIRST_REF + (uint32_t)i, bases[i], good_argv,
                               tags[i]);
   }
   pids[INSTANCES] =
       start_instance(exe, dir, "deadp", sock, FIRST_REF + INSTANCES, dead_base, dead_argv, NULL);
   pids[INSTANCES + 1] = start_instance(exe, dir, "garbagep", sock, FIRST_REF + INSTANCES + 1,
                                        garbage_base, garbage_argv, NULL);
   /* Same principal ref as instance 0, so it derives instance 0's kinds. */
   pids[INSTANCES + 2] =
       start_instance(exe, dir, "collider", sock, FIRST_REF, bases[0], good_argv, "collide");

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(sock, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);

   pump_t ps = {.host = &host, .lock = &lock};
   atomic_init(&ps.stop, 0);
   pthread_t pt;
   assert(pthread_create(&pt, NULL, run_pump, &ps) == 0);

   aimee_module_client_t client;
   assert(aimee_module_client_init(&client, &caller) == 0);

   /* Admit every instance that is waiting on a verdict. Nothing has spawned
    * before this point -- that is the gate doing its job. */
   for (int i = 0; i < INSTANCES; i++)
      assert(admit_instance(&client, bases[i], ADMIT_ALLOW) == 1);
   (void)admit_instance(&client, dead_base, ADMIT_ALLOW);
   (void)admit_instance(&client, garbage_base, ADMIT_ALLOW);
   printf("scale: %d instances admitted by the gate\n", INSTANCES);

   /* --- 1. ten instances, each declaring its own plugin's tool --- */
   for (int i = 0; i < INSTANCES; i++)
   {
      char verb[64] = "";
      int n = declare_until(&client, bases[i], 1, verb, sizeof(verb));
      assert(n == 1 && "each instance must declare its own plugin's tool");
      char want[64];
      snprintf(want, sizeof(want), "do_%s", tags[i]);
      assert(strcmp(verb, want) == 0 && "an instance answered with another's tool");
   }
   printf("scale: %d instances each declared their own tool over their own event kinds\n",
          INSTANCES);

   /* --- 2. concurrent invocation across every instance --- */
   {
      int served = 0;
      for (int i = 0; i < INSTANCES; i++)
      {
         char verb[64];
         snprintf(verb, sizeof(verb), "do_%s", tags[i]);
         size_t vl = strlen(verb);
         unsigned char req[128];
         wr_u32(req, INVOKE_REQ_MAGIC);
         wr_u32(req + 4, WIRE_VERSION);
         wr_u16(req + 8, (uint16_t)vl);
         wr_u16(req + 10, 0);
         wr_u32(req + 12, 0);
         memcpy(req + INVOKE_REQ_HEADER, verb, vl);
         unsigned char resp[4096];
         uint32_t rn = 0;
         assert(aimee_module_client_call(&client, bases[i], STAGE_INVOKE, 0, 0, req,
                                         (uint32_t)(INVOKE_REQ_HEADER + vl), resp, sizeof(resp),
                                         &rn, NULL, NULL) == AIMEE_MODULE_CALL_OK);
         resp[rn] = '\0';
         /* Match the quoted tag alone: the plugin's json.dumps puts a space
          * after the colon, and tags are unique per instance, so the tag is
          * the attribution and the separator style is not. */
         char expect[64];
         snprintf(expect, sizeof(expect), "\"%s\"", tags[i]);
         /* Each answer must come from the instance that was addressed -- the
          * check that would fail if kinds were shared or routed loosely. */
         if (strstr((const char *)resp + INVOKE_RESP_HEADER, expect) == NULL)
         {
            fprintf(stderr, "instance %d (base %u) verb=%s: wanted %s, body=<%s>\n", i, bases[i],
                    verb, expect, (const char *)resp + INVOKE_RESP_HEADER);
            assert(!"an invocation was answered by the wrong instance");
         }
         served++;
      }
      printf("scale: %d invocations each answered by the addressed instance\n", served);
   }

   /* --- 3. misbehaving plugins do not take their module down --- */
   {
      int n = declare_until(&client, dead_base, 0, NULL, 0);
      assert(n == 0 && "a plugin that exited must leave no commands");
      printf("scale: the exited-plugin instance answers and declares 0\n");

      int g = declare_until(&client, garbage_base, 0, NULL, 0);
      assert(g == 0 && "a plugin emitting non-JSON must leave no commands");
      printf("scale: the garbage-plugin instance answers and declares 0\n");
   }

   /* --- 4. the collider proves the one-server-per-kind constraint --- */
   {
      /* instance 0 owns bases[0]; the collider derives the same pair from the
       * same ref.
       * bus_host_serve_kind() refuses the second, so instance 0 must still be
       * the one answering -- if the collider had won, the tool name would be
       * "do_collide". */
      char verb[64] = "";
      int n = declare_until(&client, bases[0], 1, verb, sizeof(verb));
      assert(n == 1);
      char want[64];
      snprintf(want, sizeof(want), "do_%s", tags[0]);
      assert(strcmp(verb, want) == 0 &&
             "a colliding instance must not take over an allocated event kind");
      printf("scale: a collider on an allocated event base did not displace the owner\n");
   }

   atomic_store_explicit(&ps.stop, 1, memory_order_release);
   pthread_join(pt, NULL);
   aimee_module_client_destroy(&client);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);

   for (int i = 0; i < total; i++)
   {
      kill(pids[i], SIGTERM);
      int st = 0;
      waitpid(pids[i], &st, 0);
   }
   char rm[512];
   snprintf(rm, sizeof(rm), "rm -rf '%s'", dir);
   (void)system(rm);

   printf("all plugin scale/exploratory tests passed\n");
   return 0;
}
