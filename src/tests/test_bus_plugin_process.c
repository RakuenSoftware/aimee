/* test_bus_plugin_process.c: end-to-end over REAL processes and a REAL bus.
 *
 * Everything under test here is a seam between two things that are tested
 * separately and could still disagree:
 *
 *   - a real Go plugin-module process, started as aimee-module-mcp-NAME
 *   - a real MCP server plugin (a script) speaking JSON-RPC on its stdio
 *   - the real C bus host, runtime endpoint, and grant policy
 *   - the DCMD/DCMR declaration wire format, encoded in Go and decoded in C
 *
 * Two instances run at once, because the design's load-bearing claim is that a
 * deployment runs many. bus_host_serve_kind() binds one event kind to exactly
 * one serving slot, so instances that shared kinds would silently lose all but
 * the first -- this proves they do not share, and that each answers for its own
 * plugin.
 *
 * Usage: unit-test-bus-plugin-process /abs/path/to/aimee-module
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
#include <sys/stat.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* MUST match server-go/modules/mcp/mcp.go and src/headers/module_commands.h.
 * A plugin instance's kinds are DERIVED from its principal ref by the canonical
 * module rule, so the ref is the only per-instance allocation. */
#define PLUGIN_KIND(ref, stage) (4096u + (ref) * 256u + (stage))
#define STAGE_INVOKE            1u
#define STAGE_DECLARE           2u

#define DECL_REQUEST_MAGIC  0x444d4344u /* "DCMD" */
#define DECL_RESPONSE_MAGIC 0x524d4344u /* "DCMR" */
#define WIRE_VERSION        1u
#define DECL_HEADER_LEN     12u
#define DECL_RECORD_LEN     16u
#define DECL_PENDING_MAGIC  0x504d4344u /* "DCMP" */
#define ADMIT_ALLOW         1u
#define ADMIT_REFUSE        2u
#define ADMIT_REQUEST_LEN   44u
#define INVOKE_REQ_MAGIC    0x51504d43u /* "CMPQ" */
#define INVOKE_RESP_MAGIC   0x53504d43u /* "CMPS" */
#define INVOKE_REQ_HEADER   16u
#define INVOKE_RESP_HEADER  12u

/* Distinct principal refs; the distinct event kinds follow from them. */
#define ALPHA_REF    201u
#define BETA_REF     202u
#define INERT_REF    203u
#define PLUGGY_REF   204u
#define REFUSED_REF  205u
#define CALLER_REF   250u
#define ALPHA_BASE   PLUGIN_KIND(ALPHA_REF, STAGE_INVOKE)
#define BETA_BASE    PLUGIN_KIND(BETA_REF, STAGE_INVOKE)
#define INERT_BASE   PLUGIN_KIND(INERT_REF, STAGE_INVOKE)
#define PLUGGY_BASE  PLUGIN_KIND(PLUGGY_REF, STAGE_INVOKE)
#define REFUSED_BASE PLUGIN_KIND(REFUSED_REF, STAGE_INVOKE)

typedef struct
{
   bus_host_t *host;
   pthread_mutex_t *lock;
   atomic_int stop;
} pump_thread_t;

static void *run_pump(void *argument)
{
   pump_thread_t *state = argument;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   while (!atomic_load_explicit(&state->stop, memory_order_acquire))
   {
      pthread_mutex_lock(state->lock);
      (void)bus_host_pump(state->host);
      pthread_mutex_unlock(state->lock);
      nanosleep(&pause, NULL);
   }
   return NULL;
}

static void wait_for_admitted(bus_host_t *host, pthread_mutex_t *lock, uint32_t want)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000};
   for (int attempt = 0; attempt < 4000; ++attempt)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= want)
         return;
      nanosleep(&pause, NULL);
   }
   fprintf(stderr, "timed out waiting for %u bus clients\n", want);
   assert(!"bus admission timed out");
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
   p[0] = (unsigned char)(v & 0xff);
   p[1] = (unsigned char)((v >> 8) & 0xff);
   p[2] = (unsigned char)((v >> 16) & 0xff);
   p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void wr_u16(unsigned char *p, uint16_t v)
{
   p[0] = (unsigned char)(v & 0xff);
   p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* A minimal MCP server: initialize, tools/list, tools/call over stdio.
 *
 * Its tool names are deliberately NOT registry-legal ("search-issues",
 * "createPullRequest"), because folding them onto [a-z0-9_] is the step that
 * decides whether a real plugin's tools are reachable at all. */
static const char *FAKE_PLUGIN_PY =
    "import sys, json\n"
    "TOOLS=[{'name':'search-issues','description':'Search  issues\\nacross repos',"
    "'inputSchema':{'type':'object'}},"
    "{'name':'createPullRequest','description':'Open a PR','inputSchema':{'type':'object'}}]\n"
    "for line in sys.stdin:\n"
    "    line=line.strip()\n"
    "    if not line: continue\n"
    "    req=json.loads(line)\n"
    "    m=req.get('method'); i=req.get('id')\n"
    "    if m=='initialize': r={'protocolVersion':'2024-11-05'}\n"
    "    elif m=='tools/list': r={'tools':TOOLS}\n"
    "    elif m=='tools/call':\n"
    "        p=req.get('params',{})\n"
    "        r={'content':[{'type':'text','text':'called:'+p.get('name','')}],"
    "'echo':p.get('arguments')}\n"
    "    else: r=None\n"
    "    if r is None:\n"
    "        out={'jsonrpc':'2.0','id':i,'error':{'code':-32601,'message':'no method'}}\n"
    "    else:\n"
    "        out={'jsonrpc':'2.0','id':i,'result':r}\n"
    "    sys.stdout.write(json.dumps(out)+'\\n'); sys.stdout.flush()\n";

static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   assert(fputs(content, f) >= 0);
   assert(fclose(f) == 0);
}

/* Start one plugin-module instance. `plugin_argv_json` may be NULL for an
 * instance that hosts no plugin. */
static pid_t start_instance(const char *module_exe, const char *link_dir, const char *instance,
                            const char *socket_path, uint32_t principal_ref, uint32_t event_base,
                            const char *plugin_argv_json)
{
   char link_path[PATH_MAX];
   snprintf(link_path, sizeof(link_path), "%s/aimee-module-mcp-%s", link_dir, instance);
   unlink(link_path);
   assert(symlink(module_exe, link_path) == 0);

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
      char ref[32], base[32];
      snprintf(ref, sizeof(ref), "%u", principal_ref);
      snprintf(base, sizeof(base), "%u", event_base);
      setenv("AIMEE_MODULE_PRINCIPAL_REF", ref, 1);
      setenv("AIMEE_MODULE_EVENT_BASE", base, 1);
      /* The fixture plugins carry no MCP annotations, so their tools count as
       * `write`. The default ceiling is `read` (least privilege), which would
       * correctly withhold every one of them -- so an instance that is meant to
       * expose tools has to say so, exactly as a real deployment would. */
      setenv("AIMEE_MCP_PLUGIN_PERMISSION", "write", 1);
      if (plugin_argv_json)
         setenv("AIMEE_MCP_PLUGIN_ARGV", plugin_argv_json, 1);
      else
         unsetenv("AIMEE_MCP_PLUGIN_ARGV");
      execl(link_path, link_path, socket_path, (char *)NULL);
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
   wr_u32(request, DECL_REQUEST_MAGIC);
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
         wr_u32(admit, DECL_REQUEST_MAGIC);
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

/* Ask an instance to declare, retrying while its plugin is still coming up.
 * Returns the command count; fills groups/verbs for the caller to assert. */
static uint32_t declare_commands(aimee_module_client_t *client, uint32_t event_base,
                                 char groups[][64], char verbs[][64], char summaries[][256],
                                 uint32_t *surfaces_out, uint32_t *visibility_out, int max,
                                 int expect_at_least)
{
   unsigned char request[8];
   wr_u32(request, DECL_REQUEST_MAGIC);
   wr_u32(request + 4, WIRE_VERSION);
   unsigned char response[65536];

   for (int attempt = 0; attempt < 400; ++attempt)
   {
      uint32_t response_len = 0;
      aimee_module_call_result_t rc = aimee_module_client_call(
          client, event_base + 1, STAGE_DECLARE, 0, 0, request, sizeof(request), response,
          sizeof(response), &response_len, NULL, NULL);
      if (rc == AIMEE_MODULE_CALL_OK && response_len >= DECL_HEADER_LEN)
      {
         assert(rd_u32(response) == DECL_RESPONSE_MAGIC);
         assert(rd_u32(response + 4) == WIRE_VERSION);
         uint32_t count = rd_u32(response + 8);
         if ((int)count >= expect_at_least)
         {
            uint32_t off = DECL_HEADER_LEN;
            for (uint32_t i = 0; i < count && (int)i < max; i++)
            {
               assert(off + DECL_RECORD_LEN <= response_len);
               if (surfaces_out)
                  surfaces_out[i] = rd_u32(response + off);
               if (visibility_out)
                  visibility_out[i] = rd_u32(response + off + 4);
               uint16_t gl = rd_u16(response + off + 8);
               uint16_t vl = rd_u16(response + off + 10);
               uint16_t sl = rd_u16(response + off + 12);
               off += DECL_RECORD_LEN;
               assert(off + gl + vl + sl <= response_len);
               snprintf(groups[i], 64, "%.*s", (int)gl, response + off);
               snprintf(verbs[i], 64, "%.*s", (int)vl, response + off + gl);
               snprintf(summaries[i], 256, "%.*s", (int)sl, response + off + gl + vl);
               off += (uint32_t)gl + vl + sl;
            }
            assert(off == response_len && "declaration had trailing bytes");
            return count;
         }
      }
      const struct timespec pause = {.tv_sec = 0, .tv_nsec = 25000000};
      nanosleep(&pause, NULL);
   }
   fprintf(stderr, "declaration never reached %d commands for base %u\n", expect_at_least,
           event_base);
   assert(!"declaration timed out");
   return 0;
}

int main(int argc, char **argv)
{
   assert(argc == 2);
   char module_exe[PATH_MAX];
   assert(realpath(argv[1], module_exe) != NULL);
   char caller_exe[PATH_MAX];
   assert(realpath("/proc/self/exe", caller_exe) != NULL);

   char dir[256];
   snprintf(dir, sizeof(dir), "%s/aimee-plugin-e2e-XXXXXX", platform_tmpdir());
   assert(mkdtemp(dir) != NULL);

   char plugin_py[PATH_MAX], socket_path[512];
   snprintf(plugin_py, sizeof(plugin_py), "%s/fake_mcp.py", dir);
   snprintf(socket_path, sizeof(socket_path), "%s/module.sock", dir);
   write_file(plugin_py, FAKE_PLUGIN_PY);

   char plugin_argv[PATH_MAX + 64];
   snprintf(plugin_argv, sizeof(plugin_argv), "[\"python3\",\"%s\"]", plugin_py);

   /* Each instance is granted ONLY its own pair. A grant keys on
    * (principal_class, principal_ref) and then CHECKS the peer executable, so
    * three instances of one binary legitimately hold three different grants. */
   const uint32_t alpha_kinds[] = {ALPHA_BASE, ALPHA_BASE + 1};
   const uint32_t beta_kinds[] = {BETA_BASE, BETA_BASE + 1};
   const uint32_t inert_kinds[] = {INERT_BASE, INERT_BASE + 1};
   const uint32_t pluggy_kinds[] = {PLUGGY_BASE, PLUGGY_BASE + 1};
   const uint32_t refused_kinds[] = {REFUSED_BASE, REFUSED_BASE + 1};
   const uint32_t all_kinds[] = {ALPHA_BASE,   ALPHA_BASE + 1,  BETA_BASE,   BETA_BASE + 1,
                                 INERT_BASE,   INERT_BASE + 1,  PLUGGY_BASE, PLUGGY_BASE + 1,
                                 REFUSED_BASE, REFUSED_BASE + 1};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = ALPHA_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_exe,
        .serve = alpha_kinds,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = BETA_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_exe,
        .serve = beta_kinds,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = INERT_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_exe,
        .serve = inert_kinds,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = PLUGGY_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_exe,
        .serve = pluggy_kinds,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = REFUSED_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = module_exe,
        .serve = refused_kinds,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = caller_exe,
        .request = all_kinds,
        .request_count = 10},
   };

   bus_host_config_t host_config = {.max_slots = 8,
                                    .slot_size = 4096,
                                    .inline_budget = 3800,
                                    .queue_capacity = 32,
                                    .arena_size = 256 * 1024};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 8,
                                          .stale_after_ns = 30000000000ULL,
                                          .grants = grants,
                                          .grant_count = 6};
   bus_runtime_t *runtime = bus_runtime_start(&host, &host_lock, &runtime_config);
   assert(runtime != NULL);

   pid_t alpha =
       start_instance(module_exe, dir, "alpha", socket_path, ALPHA_REF, ALPHA_BASE, plugin_argv);
   pid_t beta =
       start_instance(module_exe, dir, "beta", socket_path, BETA_REF, BETA_BASE, plugin_argv);
   pid_t inert = start_instance(module_exe, dir, "inert", socket_path, INERT_REF, INERT_BASE, NULL);

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);

   /* Three module processes plus this caller. If instances shared event kinds
    * the bus would refuse all but the first, and this would time out. */
   wait_for_admitted(&host, &host_lock, 4);
   printf("e2e: three plugin instances and one caller admitted\n");

   pump_thread_t pump_state = {.host = &host, .lock = &host_lock};
   atomic_init(&pump_state.stop, 0);
   pthread_t pump_thread;
   assert(pthread_create(&pump_thread, NULL, run_pump, &pump_state) == 0);

   aimee_module_client_t client;
   assert(aimee_module_client_init(&client, &caller) == 0);

   char groups[8][64], verbs[8][64], summaries[8][256];
   uint32_t surfaces[8], visibility[8];

   /* Nothing has started yet: each instance is waiting for the daemon's
    * admission verdict. Deliver it, the way module_commands.c does. */
   assert(admit_instance(&client, ALPHA_BASE, ADMIT_ALLOW) == 1);
   assert(admit_instance(&client, BETA_BASE, ADMIT_ALLOW) == 1);
   printf("e2e: alpha and beta admitted by the gate\n");

   /* --- alpha declares its plugin's tools, folded onto the registry grammar --- */
   uint32_t n =
       declare_commands(&client, ALPHA_BASE, groups, verbs, summaries, surfaces, visibility, 8, 2);
   assert(n == 2);
   assert(strcmp(groups[0], "alpha") == 0);
   assert(strcmp(groups[1], "alpha") == 0);
   /* "search-issues" and "createPullRequest" are not registry-legal; the module
    * folds them, which is what makes them reachable at all. */
   assert(strcmp(verbs[0], "search_issues") == 0);
   assert(strcmp(verbs[1], "createpullrequest") == 0);
   for (int i = 0; i < 2; i++)
      for (const char *p = verbs[i]; *p; p++)
         assert((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_');
   /* The description's newline and double space are flattened: this lands in
    * line-oriented surfaces. */
   assert(strcmp(summaries[0], "Search issues across repos") == 0);
   /* MCP exposure requires a CLI route (command_registry.c:68). */
   assert((surfaces[0] & 0x1u) && (surfaces[0] & 0x4u));
   /* Plugin tools stay out of the prominent tools/list. */
   assert(visibility[0] == 1u);
   printf("e2e: alpha declared %u commands, names folded to the registry grammar\n", n);

   /* --- beta declares under its OWN group, over its OWN event kinds --- */
   char bgroups[8][64], bverbs[8][64], bsummaries[8][256];
   uint32_t bn =
       declare_commands(&client, BETA_BASE, bgroups, bverbs, bsummaries, NULL, NULL, 8, 2);
   assert(bn == 2);
   assert(strcmp(bgroups[0], "beta") == 0);
   assert(strcmp(bverbs[0], "search_issues") == 0);
   printf("e2e: beta declared %u commands under its own group\n", bn);

   /* --- an instance with no plugin declares nothing, and stays healthy --- */
   char igroups[8][64], iverbs[8][64], isummaries[8][256];
   uint32_t in_ =
       declare_commands(&client, INERT_BASE, igroups, iverbs, isummaries, NULL, NULL, 8, 0);
   assert(in_ == 0);
   printf("e2e: the plugin-less instance declared 0 commands and answered OK\n");

   /* --- invoking a declared command reaches the plugin by its ORIGINAL name --- */
   {
      const char *verb = "search_issues";
      const char *args = "{\"q\":\"bug\"}";
      size_t vl = strlen(verb), al = strlen(args);
      unsigned char req[256];
      wr_u32(req, INVOKE_REQ_MAGIC);
      wr_u32(req + 4, WIRE_VERSION);
      wr_u16(req + 8, (uint16_t)vl);
      wr_u16(req + 10, 0);
      wr_u32(req + 12, (uint32_t)al);
      memcpy(req + INVOKE_REQ_HEADER, verb, vl);
      memcpy(req + INVOKE_REQ_HEADER + vl, args, al);

      unsigned char resp[8192];
      uint32_t resp_len = 0;
      assert(aimee_module_client_call(&client, ALPHA_BASE, STAGE_INVOKE, 0, 0, req,
                                      (uint32_t)(INVOKE_REQ_HEADER + vl + al), resp, sizeof(resp),
                                      &resp_len, NULL, NULL) == AIMEE_MODULE_CALL_OK);
      assert(resp_len > INVOKE_RESP_HEADER);
      assert(rd_u32(resp) == INVOKE_RESP_MAGIC);
      assert(rd_u32(resp + 4) == WIRE_VERSION);
      assert(INVOKE_RESP_HEADER + rd_u32(resp + 8) == resp_len);
      resp[resp_len] = '\0';
      const char *body = (const char *)resp + INVOKE_RESP_HEADER;
      /* The plugin echoes the name IT was called with -- the original, not the
       * folded registry spelling. Getting this backwards makes every
       * non-conforming tool name uncallable. */
      assert(strstr(body, "called:search-issues") != NULL);
      assert(strstr(body, "bug") != NULL);
      printf("e2e: invoke reached the plugin as \"search-issues\" and echoed its arguments\n");
   }

   /* --- an undeclared verb is refused without reaching the plugin --- */
   {
      const char *verb = "no_such_tool";
      size_t vl = strlen(verb);
      unsigned char req[128];
      wr_u32(req, INVOKE_REQ_MAGIC);
      wr_u32(req + 4, WIRE_VERSION);
      wr_u16(req + 8, (uint16_t)vl);
      wr_u16(req + 10, 0);
      wr_u32(req + 12, 0);
      memcpy(req + INVOKE_REQ_HEADER, verb, vl);
      unsigned char resp[512];
      uint32_t resp_len = 0;
      aimee_module_call_result_t rc = aimee_module_client_call(
          &client, ALPHA_BASE, STAGE_INVOKE, 0, 0, req, (uint32_t)(INVOKE_REQ_HEADER + vl), resp,
          sizeof(resp), &resp_len, NULL, NULL);
      assert(rc == AIMEE_MODULE_CALL_INVALID_REQUEST);
      printf("e2e: an undeclared verb was refused\n");
   }

   /* --- a malformed frame is refused rather than read past --- */
   {
      unsigned char req[INVOKE_REQ_HEADER];
      wr_u32(req, INVOKE_REQ_MAGIC);
      wr_u32(req + 4, WIRE_VERSION);
      wr_u16(req + 8, 8); /* claims a verb the frame does not carry */
      wr_u16(req + 10, 0);
      wr_u32(req + 12, 0);
      unsigned char resp[512];
      uint32_t resp_len = 0;
      aimee_module_call_result_t rc =
          aimee_module_client_call(&client, ALPHA_BASE, STAGE_INVOKE, 0, 0, req, sizeof(req), resp,
                                   sizeof(resp), &resp_len, NULL, NULL);
      assert(rc == AIMEE_MODULE_CALL_INVALID_REQUEST);
      printf("e2e: a length-mismatched invoke frame was refused\n");
   }

   /* --- a REFUSED plugin never executes ---
    *
    * This is the property Slice 4 exists for, and it has to be checked by
    * OBSERVING that the code did not run, not by trusting a status code. The
    * refused instance's plugin would create a sentinel file as its very first
    * action; if the file never appears, the process never started. */
   {
      char sentinel[PATH_MAX], refused_py[PATH_MAX], refused_argv[PATH_MAX * 2];
      snprintf(sentinel, sizeof(sentinel), "%s/REFUSED_PLUGIN_RAN", dir);
      snprintf(refused_py, sizeof(refused_py), "%s/refused.py", dir);
      {
         char body[PATH_MAX + 128];
         snprintf(body, sizeof(body), "open(%c%s%c,'w').write('ran')\n", '"', sentinel, '"');
         write_file(refused_py, body);
      }
      snprintf(refused_argv, sizeof(refused_argv), "[\"python3\",\"%s\"]", refused_py);

      pid_t refused = start_instance(module_exe, dir, "refused", socket_path, REFUSED_REF,
                                     REFUSED_BASE, refused_argv);
      assert(admit_instance(&client, REFUSED_BASE, ADMIT_REFUSE) == 1);

      /* Give a plugin that WAS going to start ample time to prove it. */
      const struct timespec settle = {.tv_sec = 1, .tv_nsec = 0};
      nanosleep(&settle, NULL);

      assert(access(sentinel, F_OK) != 0 &&
             "a refused plugin executed anyway -- the gate does not gate");

      char rg[8][64], rv[8][64], rs[8][256];
      int rn_cmds = (int)declare_commands(&client, REFUSED_BASE, rg, rv, rs, NULL, NULL, 8, 0);
      assert(rn_cmds == 0 && "a refused plugin must contribute no commands");
      printf("e2e: a refused plugin never executed and declared nothing\n");

      kill(refused, SIGTERM);
      int rst = 0;
      waitpid(refused, &rst, 0);
   }

   /* --- a PLUGGY plugin travels the identical path, with no pluggy-specific
    *     code anywhere above the host shim ---
    *
    * The instance below runs scripts/aimee-pluggy-host.py, which serves a real
    * pluggy plugin as an MCP server. It is started by the SAME Go module, over
    * the SAME stages, and its hooks arrive as commands through the SAME
    * declaration format. That is the claim "pluggy is not a protocol" being
    * checked rather than asserted.
    *
    * Skipped when the harness is not told where the shim and pluggy live, so
    * this binary still runs standalone. */
   {
      const char *pluggy_host = getenv("AIMEE_PLUGGY_HOST");
      const char *pluggy_path = getenv("AIMEE_PLUGGY_PYTHONPATH");
      if (pluggy_host && pluggy_host[0] && pluggy_path && pluggy_path[0])
      {
         char pluggy_argv[PATH_MAX * 2];
         snprintf(pluggy_argv, sizeof(pluggy_argv),
                  "[\"python3\",\"%s\",\"--project\",\"aimee_demo\",\"--spec-module\","
                  "\"aimee_demo_spec\",\"--plugin-module\",\"aimee_demo_plugin\"]",
                  pluggy_host);
         setenv("PYTHONPATH", pluggy_path, 1);
         pid_t pluggy = start_instance(module_exe, dir, "pluggy", socket_path, PLUGGY_REF,
                                       PLUGGY_BASE, pluggy_argv);
         assert(admit_instance(&client, PLUGGY_BASE, ADMIT_ALLOW) == 1);
         char pg[8][64], pv[8][64], psum[8][256];
         uint32_t pn = declare_commands(&client, PLUGGY_BASE, pg, pv, psum, NULL, NULL, 8, 2);
         assert(pn == 2 && "the pluggy plugin's two callable hooks must arrive as commands");
         assert(strcmp(pg[0], "pluggy") == 0);
         /* greet and pick_one, in reflection order; the wrapper-only and
          * unimplemented hooks must NOT be here. */
         int saw_greet = 0, saw_pick = 0;
         for (uint32_t i = 0; i < pn; i++)
         {
            if (strcmp(pv[i], "greet") == 0)
               saw_greet = 1;
            if (strcmp(pv[i], "pick_one") == 0)
               saw_pick = 1;
            assert(strcmp(pv[i], "wrapped_only") != 0);
            assert(strcmp(pv[i], "never_implemented") != 0);
         }
         assert(saw_greet && saw_pick);

         /* Invoke a hook through the ordinary invoke stage. */
         const char *verb = "greet";
         const char *args = "{\"name\":\"ada\"}";
         size_t vl = strlen(verb), al = strlen(args);
         unsigned char req[256];
         wr_u32(req, INVOKE_REQ_MAGIC);
         wr_u32(req + 4, WIRE_VERSION);
         wr_u16(req + 8, (uint16_t)vl);
         wr_u16(req + 10, 0);
         wr_u32(req + 12, (uint32_t)al);
         memcpy(req + INVOKE_REQ_HEADER, verb, vl);
         memcpy(req + INVOKE_REQ_HEADER + vl, args, al);
         unsigned char resp[8192];
         uint32_t rn = 0;
         assert(aimee_module_client_call(&client, PLUGGY_BASE, STAGE_INVOKE, 0, 0, req,
                                         (uint32_t)(INVOKE_REQ_HEADER + vl + al), resp,
                                         sizeof(resp), &rn, NULL, NULL) == AIMEE_MODULE_CALL_OK);
         resp[rn] = '\0';
         assert(strstr((const char *)resp + INVOKE_RESP_HEADER, "hello ada") != NULL);
         printf("e2e: a pluggy plugin declared and invoked over the unchanged MCP module\n");

         kill(pluggy, SIGTERM);
         int pst = 0;
         waitpid(pluggy, &pst, 0);
      }
      else
      {
         printf("e2e: pluggy leg skipped (set AIMEE_PLUGGY_HOST and "
                "AIMEE_PLUGGY_PYTHONPATH to run it)\n");
      }
   }

   /* --- alpha's plugin dying withdraws its commands, and beta is unaffected --- */
   {
      /* The plugin is a grandchild (module -> python). Kill the whole process
       * group of the module's child by matching the script path, scoped to this
       * run's temp dir so nothing else on the host is touched. */
      char kill_cmd[PATH_MAX + 64];
      snprintf(kill_cmd, sizeof(kill_cmd), "pkill -f '%s' >/dev/null 2>&1", plugin_py);
      (void)system(kill_cmd);

      /* alpha and beta both lose their plugin (same script path), so both go to
       * zero. What matters is that the module PROCESSES survive and keep
       * answering -- a dead plugin must not take its module down. */
      char zg[8][64], zv[8][64], zs[8][256];
      uint32_t zn = declare_commands(&client, ALPHA_BASE, zg, zv, zs, NULL, NULL, 8, 0);
      /* The module may not have noticed yet; retry until it reports zero. */
      for (int attempt = 0; attempt < 200 && zn != 0; ++attempt)
      {
         const struct timespec pause = {.tv_sec = 0, .tv_nsec = 25000000};
         nanosleep(&pause, NULL);
         zn = declare_commands(&client, ALPHA_BASE, zg, zv, zs, NULL, NULL, 8, 0);
      }
      assert(zn == 0 && "a dead plugin must withdraw its commands");
      printf("e2e: alpha's dead plugin withdrew its commands, module still answering\n");
   }

   atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
   pthread_join(pump_thread, NULL);
   aimee_module_client_destroy(&client);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);

   /* Scoped teardown only: this run's children and this run's directory. */
   kill(alpha, SIGTERM);
   kill(beta, SIGTERM);
   kill(inert, SIGTERM);
   int status = 0;
   waitpid(alpha, &status, 0);
   waitpid(beta, &status, 0);
   waitpid(inert, &status, 0);
   char rm_cmd[512];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", dir);
   (void)system(rm_cmd);

   printf("all plugin process e2e tests passed\n");
   return 0;
}
