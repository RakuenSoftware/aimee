/* test_sandbox_learned_observe.c: the C half of the learned-toolchain write
 * path -- everything sandbox_learned_observe() does BEFORE the bus carries the
 * request away.
 *
 * test_module_runtime.c's sandbox leg pins the far half: a real Go module
 * process, attached to a real bus, decoding an OBSERVE request and persisting
 * it. What that cannot see is whether this side ever calls, or what it sends.
 * The prefilter, the config gate, the git-root resolution and the field names
 * are all upstream of the wire, so a bug in any of them leaves the wire test
 * green while nothing is ever learned.
 *
 * That failure is not hypothetical. A delegate turn was run against a live
 * server to exercise this path and recorded nothing, because the working
 * directory it passed did not resolve to a git root -- a gate this test now
 * pins directly, for the cost of a fork rather than a model call.
 *
 * The bus is faked here (the two obs_bus entry points are defined below), which
 * is what makes the SUPPRESSION cases assertable: "no call was made" is not
 * observable from the far side of a real bus. git_repo_root() is real, because
 * whether a directory resolves to a repository is precisely what tripped.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/core/event_bus/module_client.h>
#include <aimee/sandbox/module_api.h>

#include "cJSON.h"
#include "sandbox_learned.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* ---- the faked collaborators -------------------------------------------- */

static int g_gate = 1; /* stands in for config_delegate_sandbox_learn_packages */
int config_delegate_sandbox_learn_packages(void)
{
   return g_gate;
}

static int g_available = 1;
static int g_calls;
static uint32_t g_kind, g_stage;
static char g_body[4096];

int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return g_available;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id, (void)cancelled, (void)cancel_context;
   /* A learning call must never be able to hold a delegate turn open. */
   assert(deadline_ns != 0);
   g_calls++;
   g_kind = event_kind;
   g_stage = stage_id;
   assert(request_len < sizeof g_body);
   memcpy(g_body, request_body, request_len);
   g_body[request_len] = '\0';

   /* Answer the way the module does, so the caller's reply handling is exercised
    * rather than short-circuited by an error path. */
   const char *reply = "{\"parsed\":1,\"recorded\":1,\"packages\":[\"tree\"]}";
   uint32_t len = (uint32_t)strlen(reply);
   if (len > response_capacity)
      return AIMEE_MODULE_CALL_TRANSPORT;
   memcpy(response_body, reply, len);
   if (response_len)
      *response_len = len;
   return AIMEE_MODULE_CALL_OK;
}

/* ---- helpers ------------------------------------------------------------- */

static char repo[512];

static void run(const char *fmt, ...)
{
   char cmd[1024];
   va_list ap;
   va_start(ap, fmt);
   assert(vsnprintf(cmd, sizeof cmd, fmt, ap) < (int)sizeof cmd);
   va_end(ap);
   assert(system(cmd) == 0);
}

static void reset(void)
{
   g_calls = 0;
   g_kind = g_stage = 0;
   g_body[0] = '\0';
   g_gate = 1;
   g_available = 1;
}

static const char *field(const char *name)
{
   static char value[1024];
   cJSON *doc = cJSON_Parse(g_body);
   assert(doc != NULL);
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, name));
   assert(s != NULL);
   snprintf(value, sizeof value, "%s", s);
   cJSON_Delete(doc);
   return value;
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-learned-observe-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   assert(snprintf(repo, sizeof repo, "%s/repo", home) > 0);
   run("git init -q '%s'", repo);
   /* A nested directory: the payload must carry the repository ROOT, not the
    * working directory, or every subdirectory would learn its own toolchain. */
   char nested[640];
   assert(snprintf(nested, sizeof nested, "%s/src/deep", repo) > 0);
   run("mkdir -p '%s'", nested);

   /* 1. The real thing: a delegate-shaped command inside a repository. */
   reset();
   sandbox_learned_observe(repo, "apt-get install -y tree");
   assert(g_calls == 1);
   assert(g_kind == AIMEE_SANDBOX_EVENT_OBSERVE);
   assert(g_stage == AIMEE_SANDBOX_STAGE_OBSERVE);
   /* The module keys its store on git_root and parses command: both field names
    * are load-bearing, and a rename on either side silently stops learning. */
   assert(strcmp(field("command"), "apt-get install -y tree") == 0);
   assert(strstr(field("git_root"), "/repo") != NULL);

   /* 2. Called from a subdirectory, the payload still carries the root. */
   reset();
   sandbox_learned_observe(nested, "apt-get install -y ripgrep");
   assert(g_calls == 1);
   const char *root = field("git_root");
   assert(strstr(root, "/repo") != NULL && strstr(root, "deep") == NULL);

   /* 3. The prefilter. Neither word present, and an incidental "apt" inside a
    * longer word, must both stay free -- resolving a git root forks a process,
    * so this is the check that keeps ordinary commands cheap. */
   reset();
   sandbox_learned_observe(repo, "echo hello");
   assert(g_calls == 0);
   sandbox_learned_observe(repo, "echo chapter adapter");
   assert(g_calls == 0);
   /* "install" alone, and "apt" alone, are each insufficient. */
   sandbox_learned_observe(repo, "npm install left-pad");
   assert(g_calls == 0);
   sandbox_learned_observe(repo, "apt-cache search tree");
   assert(g_calls == 0);

   /* 4. The config gate: opting out silences the path entirely. */
   reset();
   g_gate = 0;
   sandbox_learned_observe(repo, "apt-get install -y tree");
   assert(g_calls == 0);

   /* 5. The git gate -- the one the live delegate run tripped. An install that
    * cannot be attributed to a project must not be sent at all. */
   reset();
   sandbox_learned_observe("/tmp", "apt-get install -y nmap");
   assert(g_calls == 0);

   /* 6. No module attached: the caller must not fabricate a call, and must
    * return quietly rather than surfacing anything into a delegate turn. */
   reset();
   g_available = 0;
   sandbox_learned_observe(repo, "apt-get install -y tree");
   assert(g_calls == 0);

   /* 7. Degenerate inputs are rejected before anything is resolved. */
   reset();
   sandbox_learned_observe(NULL, "apt-get install -y tree");
   sandbox_learned_observe(repo, NULL);
   sandbox_learned_observe("", "apt-get install -y tree");
   sandbox_learned_observe(repo, "");
   assert(g_calls == 0);

   run("rm -rf '%s'", home);
   printf("sandbox learned observe: prefilter, gate, git root, and payload passed\n");
   return 0;
}
