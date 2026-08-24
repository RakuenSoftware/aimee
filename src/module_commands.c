/* module_commands.c: see headers/module_commands.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "headers/module_commands.h"

#include "aimee/audit/obs_bus.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include "command_registry.h"
#include "headers/module_json_call.h"
#include "log.h"
#include "aimee/protocols/mcp/mcp_osv_gate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Declaration wire format. MUST match server-go/modules/memory/commands.go and
 * server-go/modules/mcp/commands.go -- one decoder for both, which is the point:
 * a second format for plugin commands would rebuild the divergence the registry
 * removed.
 *
 *   request  magic u32 | version u32                      (8 bytes, no payload)
 *   response magic u32 | version u32 | count u32
 *            then per command:
 *              surfaces u32 | visibility u32
 *              group_len u16 | verb_len u16 | summary_len u16 | pad u16
 *              group bytes | verb bytes | summary bytes
 *
 * Little-endian; strings are NOT NUL-terminated, the length prefixes rule. */
#define DECL_REQUEST_MAGIC  0x444d4344u /* "DCMD" */
#define DECL_RESPONSE_MAGIC 0x524d4344u /* "DCMR" */
#define DECL_WIRE_VERSION   1u
#define DECL_HEADER_LEN     12u
#define DECL_RECORD_LEN     16u

/* ADMISSION. A plugin module does not start its plugin on its own: it reports
 * the command it WANTS to run and waits for this file to answer.
 *
 *   response DCMP | version | argv_len u32 | argv     (NUL-separated argv)
 *   request  DCMD | version | verdict u32 | argv_sha256[32]
 *
 * The gate itself is mcp_osv_gate_blocks_argv() -- the SAME function the
 * aimee.yaml client registry has always used, not a second copy of the policy.
 * The hash binds the verdict to the argv that was scanned, so a module cannot
 * report one command, collect an admit, and run another. */
#define DECL_PENDING_MAGIC   0x504d4344u /* "DCMP" */
#define ADMIT_ALLOW          1u
#define ADMIT_REFUSE         2u
#define ADMIT_REQUEST_LEN    (8u + 4u + 32u)
#define PLUGIN_ARGV_MAX_LEN  8192u
#define PLUGIN_ARGV_MAX_ARGS 64

/* A declaration answer is a handful of short strings per command. 256 KiB is far
 * above any plausible plugin and still bounded. */
#define DECL_MAX_RESPONSE (256u * 1024u)

/* Invoke wire format for a plugin command. MUST match server-go/modules/mcp/mcp.go. */
#define INVOKE_REQUEST_MAGIC   0x51504d43u /* "CMPQ" */
#define INVOKE_RESPONSE_MAGIC  0x53504d43u /* "CMPS" */
#define INVOKE_REQUEST_HEADER  16u
#define INVOKE_RESPONSE_HEADER 12u
#define INVOKE_MAX_RESPONSE    (1024u * 1024u)
#define INVOKE_TIMEOUT_MS      60000

/* One registered command's owned backing.
 *
 * The registry stores BORROWED const char * (command_registry.h: "borrowed,
 * owned by the registrant"), so every string it points at has to outlive the
 * registration. These are freed only after aimee_command_unregister_module has
 * dropped the entries that point at them. */
typedef struct
{
   char *group;
   char *verb;
   char *summary;
   char *module;         /* registry `module` field, also the withdrawal key */
   uint32_t invoke_kind; /* 0 for a fixed module: not bus-dispatchable here */
   char verb_wire[128];  /* verb as sent to the module, for the invoke frame */
} owned_command_t;

/* An array of POINTERS, not of structs.
 *
 * Each registration hands the registry `ud = &owned`, and the registry keeps
 * that pointer for the life of the command. If the backing were an array of
 * structs, the realloc in owned_push() would move every entry and leave every
 * previously registered ud dangling -- a use-after-free on the next invoke.
 * Individually allocated entries keep each ud stable no matter how the index
 * grows. */
static owned_command_t **g_owned;
static size_t g_owned_count;
static size_t g_owned_cap;
static size_t g_plugin_count;

/* Per-instance state observed by the last collect, for the operator surface.
 * An instance is keyed by its principal ref, which is its permanent identity. */
#define PLUGIN_STATUS_MAX 64
static aimee_plugin_status_t g_status[PLUGIN_STATUS_MAX];
static int g_status_count;

const char *aimee_plugin_state_name(aimee_plugin_state_t state)
{
   switch (state)
   {
   case AIMEE_PLUGIN_STATE_PENDING:
      return "pending";
   case AIMEE_PLUGIN_STATE_REFUSED:
      return "refused";
   case AIMEE_PLUGIN_STATE_ACTIVE:
      return "active";
   case AIMEE_PLUGIN_STATE_SILENT:
      return "silent";
   case AIMEE_PLUGIN_STATE_ERROR:
      return "error";
   case AIMEE_PLUGIN_STATE_ABSENT:
   default:
      return "absent";
   }
}

/* Record what one instance looked like on this pass. */
static void status_record(uint32_t ref, aimee_plugin_state_t state, int commands, const char *group,
                          const char *error)
{
   if (g_status_count >= PLUGIN_STATUS_MAX)
      return;
   aimee_plugin_status_t *s = &g_status[g_status_count++];
   memset(s, 0, sizeof *s);
   s->principal_ref = ref;
   s->state = state;
   s->command_count = commands;
   if (group && group[0])
      snprintf(s->group, sizeof s->group, "%s", group);
   if (error && error[0])
      snprintf(s->last_error, sizeof s->last_error, "%s", error);
}

int aimee_module_commands_snapshot(aimee_plugin_status_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   int n = g_status_count < max ? g_status_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_status[i];
   return n;
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

/* --- plugin command dispatch ------------------------------------------------ */

/* Handler for a command that lives in a plugin module.
 *
 * Encodes the invoke frame, calls the instance's invoke stage, and returns the
 * plugin's MCP result object. Returns NULL on any failure; the surface that
 * called decides what that means, exactly as module_json_call.c does. */
static cJSON *plugin_command_invoke(const cJSON *args, void *ud)
{
   const owned_command_t *cmd = (const owned_command_t *)ud;
   if (!cmd || cmd->invoke_kind == 0)
      return NULL;

   char *args_json = NULL;
   size_t args_len = 0;
   if (args)
   {
      args_json = cJSON_PrintUnformatted((cJSON *)args);
      if (!args_json)
         return NULL;
      args_len = strlen(args_json);
   }

   size_t verb_len = strlen(cmd->verb_wire);
   size_t request_len = INVOKE_REQUEST_HEADER + verb_len + args_len;
   unsigned char *request = malloc(request_len);
   if (!request)
   {
      free(args_json);
      return NULL;
   }
   wr_u32(request, INVOKE_REQUEST_MAGIC);
   wr_u32(request + 4, DECL_WIRE_VERSION);
   wr_u16(request + 8, (uint16_t)verb_len);
   wr_u16(request + 10, 0);
   wr_u32(request + 12, (uint32_t)args_len);
   memcpy(request + INVOKE_REQUEST_HEADER, cmd->verb_wire, verb_len);
   if (args_len)
      memcpy(request + INVOKE_REQUEST_HEADER + verb_len, args_json, args_len);
   free(args_json);

   unsigned char *response = malloc(INVOKE_MAX_RESPONSE);
   if (!response)
   {
      free(request);
      return NULL;
   }
   uint32_t response_len = 0;
   aimee_module_call_result_t rc = obs_bus_module_call(
       cmd->invoke_kind, AIMEE_PLUGIN_STAGE_INVOKE, 0,
       aimee_module_call_deadline_ns(INVOKE_TIMEOUT_MS), request, (uint32_t)request_len, response,
       INVOKE_MAX_RESPONSE, &response_len, NULL, NULL);
   free(request);

   if (rc != AIMEE_MODULE_CALL_OK)
   {
      LOG_WARN("commands", "%s.%s: plugin invoke failed: %s", cmd->group, cmd->verb,
               aimee_module_call_result_name(rc));
      free(response);
      return NULL;
   }
   if (response_len < INVOKE_RESPONSE_HEADER || rd_u32(response) != INVOKE_RESPONSE_MAGIC ||
       rd_u32(response + 4) != DECL_WIRE_VERSION)
   {
      LOG_WARN("commands", "%s.%s: plugin returned a malformed frame", cmd->group, cmd->verb);
      free(response);
      return NULL;
   }
   uint32_t body_len = rd_u32(response + 8);
   if ((uint64_t)INVOKE_RESPONSE_HEADER + body_len != response_len)
   {
      LOG_WARN("commands", "%s.%s: plugin frame length disagrees with its header", cmd->group,
               cmd->verb);
      free(response);
      return NULL;
   }
   cJSON *parsed = cJSON_ParseWithLength((const char *)response + INVOKE_RESPONSE_HEADER, body_len);
   free(response);
   return parsed;
}

/* --- owned-string bookkeeping ---------------------------------------------- */

static owned_command_t *owned_push(void)
{
   if (g_owned_count == g_owned_cap)
   {
      size_t cap = g_owned_cap ? g_owned_cap * 2 : 32;
      owned_command_t **grown = realloc(g_owned, cap * sizeof *grown);
      if (!grown)
         return NULL;
      g_owned = grown;
      g_owned_cap = cap;
   }
   owned_command_t *slot = calloc(1, sizeof *slot);
   if (!slot)
      return NULL;
   g_owned[g_owned_count++] = slot;
   return slot;
}

/* Free one entry's strings and the entry itself. The caller must already have
 * removed its registry entry -- freeing while registered leaves the registry
 * pointing at freed strings. */
static void owned_free_one(owned_command_t *c)
{
   if (!c)
      return;
   free(c->group);
   free(c->verb);
   free(c->summary);
   free(c->module);
   free(c);
}

/* Drop the most recently pushed entry, for a partially built registration that
 * was refused or ran out of memory. */
static void owned_pop_last(void)
{
   if (g_owned_count == 0)
      return;
   owned_free_one(g_owned[--g_owned_count]);
}

/* Drop this module's owned strings AFTER its registry entries are gone. */
static void owned_release_module(const char *module)
{
   size_t kept = 0;
   for (size_t i = 0; i < g_owned_count; i++)
   {
      if (g_owned[i]->module && strcmp(g_owned[i]->module, module) == 0)
      {
         owned_free_one(g_owned[i]);
         continue;
      }
      if (kept != i)
         g_owned[kept] = g_owned[i];
      kept++;
   }
   g_owned_count = kept;
}

static char *dup_n(const unsigned char *p, size_t n)
{
   char *out = malloc(n + 1);
   if (!out)
      return NULL;
   if (n)
      memcpy(out, p, n);
   out[n] = '\0';
   return out;
}

/* --- one module's declaration ---------------------------------------------- */

/* Ask one module for its commands and register them under `module_id`.
 *
 * `invoke_kind` is the event kind its invoke stage listens on, or 0 for a module
 * whose commands are not dispatched through this file (a fixed module owns its
 * own handlers; this driver only makes them REACHABLE by registering them).
 *
 * Returns the number registered, or -1 when the module could not be read. The
 * module's previous commands are withdrawn first, so a repeat call converges. */
/* Run the shared OSV gate over a module's pending argv and answer it.
 *
 * The NUL-separated argv is split into a vector, scanned, and the verdict is
 * sent back on the same stage bound to a hash of exactly those bytes. Returns 1
 * when a verdict was delivered (so the caller should re-read the declaration),
 * 0 when it could not be. */
static int admit_pending(uint32_t declare_kind, const char *module_id, const unsigned char *body,
                         uint32_t body_len, int *blocked_out)
{
   if (body_len == 0 || body_len > PLUGIN_ARGV_MAX_LEN)
   {
      LOG_WARN("commands", "module %s pending argv is %u bytes; refusing", module_id, body_len);
      return 0;
   }

   /* Split on NUL into a NULL-terminated vector for the gate. */
   char *joined = malloc((size_t)body_len + 1);
   if (!joined)
      return 0;
   memcpy(joined, body, body_len);
   joined[body_len] = '\0';

   const char *argv[PLUGIN_ARGV_MAX_ARGS + 1];
   int argc = 0;
   for (uint32_t off = 0; off <= body_len && argc < PLUGIN_ARGV_MAX_ARGS;)
   {
      argv[argc++] = joined + off;
      uint32_t next = off;
      while (next < body_len && joined[next] != '\0')
         next++;
      if (next >= body_len)
         break;
      off = next + 1;
   }
   argv[argc] = NULL;

   int blocked = mcp_osv_gate_blocks_argv(module_id, argc, argv);
   free(joined);
   if (blocked_out)
      *blocked_out = blocked;

   unsigned char digest[32];
   if (aimee_sha256_raw(body, body_len, digest) != 0)
   {
      LOG_WARN("commands", "module %s: cannot hash pending argv; refusing", module_id);
      return 0;
   }

   unsigned char request[ADMIT_REQUEST_LEN];
   wr_u32(request, DECL_REQUEST_MAGIC);
   wr_u32(request + 4, DECL_WIRE_VERSION);
   wr_u32(request + 8, blocked ? ADMIT_REFUSE : ADMIT_ALLOW);
   memcpy(request + 12, digest, sizeof digest);

   /* The module answers this call with its (now possibly non-empty) declaration.
    * Give it a real buffer: treating RESPONSE_TOO_LARGE as the success path
    * would make an error code load-bearing, and it would start failing the day
    * the declaration got smaller than the buffer. The body is discarded here --
    * the caller re-reads the declaration cleanly -- but it has to fit. Admitting
    * spawns a process and completes an MCP handshake, so the deadline is
    * generous next to the 5s a plain declaration gets. */
   unsigned char *ack = malloc(DECL_MAX_RESPONSE);
   if (!ack)
      return 0;
   uint32_t out_len = 0;
   aimee_module_call_result_t rc = obs_bus_module_call(
       declare_kind, AIMEE_PLUGIN_STAGE_DECLARE, 0, aimee_module_call_deadline_ns(60000), request,
       sizeof request, ack, DECL_MAX_RESPONSE, &out_len, NULL, NULL);
   free(ack);
   if (rc != AIMEE_MODULE_CALL_OK)
   {
      LOG_WARN("commands", "module %s: admission verdict not delivered: %s", module_id,
               aimee_module_call_result_name(rc));
      return 0;
   }
   LOG_INFO("commands", "module %s: plugin %s by the OSV gate", module_id,
            blocked ? "REFUSED" : "admitted");
   return 1;
}

static int collect_one(uint32_t declare_kind, uint32_t invoke_kind, const char *module_id,
                       aimee_plugin_state_t *state_out, char *err_out, size_t err_cap)
{
   int refused = 0;
   if (state_out)
      *state_out = AIMEE_PLUGIN_STATE_ERROR;
   unsigned char request[8];
   wr_u32(request, DECL_REQUEST_MAGIC);
   wr_u32(request + 4, DECL_WIRE_VERSION);

   unsigned char *response = malloc(DECL_MAX_RESPONSE);
   if (!response)
      return -1;
   uint32_t response_len = 0;

   /* Up to two rounds: the first may come back asking for an admission verdict,
    * and the second is the declaration that follows it. One retry only -- a
    * module that keeps asking is misbehaving, and looping would spin the gate. */
   int rounds = 0;
   aimee_module_call_result_t rc;
   for (;;)
   {
      response_len = 0;
      rc = obs_bus_module_call(declare_kind, AIMEE_PLUGIN_STAGE_DECLARE, 0,
                               aimee_module_call_deadline_ns(5000), request, sizeof request,
                               response, DECL_MAX_RESPONSE, &response_len, NULL, NULL);
      if (rc != AIMEE_MODULE_CALL_OK)
      {
         LOG_WARN("commands", "module %s declaration failed: %s", module_id,
                  aimee_module_call_result_name(rc));
         if (err_out)
            snprintf(err_out, err_cap, "declaration failed: %s", aimee_module_call_result_name(rc));
         free(response);
         return -1;
      }
      if (response_len >= DECL_HEADER_LEN && rd_u32(response) == DECL_PENDING_MAGIC &&
          rd_u32(response + 4) == DECL_WIRE_VERSION && rounds == 0)
      {
         uint32_t argv_len = rd_u32(response + 8);
         if ((uint64_t)DECL_HEADER_LEN + argv_len != response_len)
         {
            LOG_WARN("commands", "module %s pending record length disagrees with its header",
                     module_id);
            if (err_out)
               snprintf(err_out, err_cap, "malformed pending record");
            free(response);
            return -1;
         }
         rounds++;
         if (!admit_pending(declare_kind, module_id, response + DECL_HEADER_LEN, argv_len,
                            &refused))
         {
            /* The verdict could not be delivered, so nothing was started and the
             * instance is still waiting rather than broken. */
            if (state_out)
               *state_out = AIMEE_PLUGIN_STATE_PENDING;
            if (err_out)
               snprintf(err_out, err_cap, "admission verdict not delivered");
            free(response);
            return -1;
         }
         continue; /* re-read the declaration now that a verdict has been applied */
      }
      break;
   }

   if (response_len < DECL_HEADER_LEN || rd_u32(response) != DECL_RESPONSE_MAGIC ||
       rd_u32(response + 4) != DECL_WIRE_VERSION)
   {
      LOG_WARN("commands", "module %s sent a malformed declaration header", module_id);
      if (err_out)
         snprintf(err_out, err_cap, "malformed declaration header");
      free(response);
      return -1;
   }
   uint32_t count = rd_u32(response + 8);

   /* Withdraw before re-registering: a duplicate (group, verb) is refused, so a
    * module whose tool set changed would otherwise never converge. */
   aimee_command_unregister_module(module_id);
   owned_release_module(module_id);

   int registered = 0;
   uint32_t off = DECL_HEADER_LEN;
   for (uint32_t i = 0; i < count; i++)
   {
      if ((uint64_t)off + DECL_RECORD_LEN > response_len)
      {
         LOG_WARN("commands", "module %s declaration truncated at record %u", module_id, i);
         break;
      }
      uint32_t surfaces = rd_u32(response + off);
      uint32_t visibility = rd_u32(response + off + 4);
      uint16_t gl = rd_u16(response + off + 8);
      uint16_t vl = rd_u16(response + off + 10);
      uint16_t sl = rd_u16(response + off + 12);
      off += DECL_RECORD_LEN;
      if ((uint64_t)off + gl + vl + sl > response_len)
      {
         LOG_WARN("commands", "module %s declaration strings truncated at record %u", module_id, i);
         break;
      }
      const unsigned char *gp = response + off;
      const unsigned char *vp = gp + gl;
      const unsigned char *sp = vp + vl;
      off += (uint32_t)gl + vl + sl;

      if (vl >= sizeof(((owned_command_t *)0)->verb_wire))
      {
         LOG_WARN("commands", "module %s: verb too long, skipped", module_id);
         continue;
      }

      owned_command_t *owned = owned_push();
      if (!owned)
         break;
      owned->group = dup_n(gp, gl);
      owned->verb = dup_n(vp, vl);
      owned->summary = dup_n(sp, sl);
      owned->module = dup_n((const unsigned char *)module_id, strlen(module_id));
      owned->invoke_kind = invoke_kind;
      memcpy(owned->verb_wire, vp, vl);
      owned->verb_wire[vl] = '\0';
      if (!owned->group || !owned->verb || !owned->summary || !owned->module)
      {
         owned_pop_last();
         break;
      }

      aimee_command_t cmd;
      memset(&cmd, 0, sizeof cmd);
      cmd.group = owned->group;
      cmd.verb = owned->verb;
      cmd.summary = owned->summary;
      cmd.schema = NULL;
      cmd.surfaces = surfaces;
      cmd.mcp_visibility = (aimee_mcp_visibility_t)visibility;
      cmd.fn = invoke_kind ? plugin_command_invoke : NULL;
      cmd.ud = owned;
      cmd.module = owned->module;

      /* A fixed module owns its own handler and is registered elsewhere; this
       * driver only dispatches PLUGIN commands. Registering a fixed command
       * with a NULL fn would be refused anyway (command_registry.c:48). */
      if (!cmd.fn)
      {
         owned_pop_last();
         continue;
      }
      if (aimee_command_register(&cmd) != 0)
      {
         /* register() already logged why. Drop the backing we just took. */
         owned_pop_last();
         continue;
      }
      registered++;
   }

   /* Distinguish the three ways an instance ends up with no commands. From
    * outside they look identical, and they need completely different actions:
    * refused means fix the package or the allowlist, silent means the plugin
    * died, active-with-zero means it genuinely offers nothing. */
   if (state_out)
   {
      if (refused)
         *state_out = AIMEE_PLUGIN_STATE_REFUSED;
      else if (registered > 0)
         *state_out = AIMEE_PLUGIN_STATE_ACTIVE;
      else
         *state_out = AIMEE_PLUGIN_STATE_SILENT;
   }
   if (refused && err_out)
      snprintf(err_out, err_cap, "blocked by the supply-chain gate");
   return registered;
}

/* --- public ----------------------------------------------------------------- */

int aimee_module_commands_collect(void)
{
   int total = 0;
   g_plugin_count = 0;
   g_status_count = 0;

   /* Plugin instances: probe the reserved principal-ref band for an attached
    * declare stage, deriving each candidate's kinds by the canonical rule.
    * obs_bus_module_available() is an in-memory slot check, so probing the whole
    * band costs nothing and needs no second source of truth for which instances
    * a deployment happens to be running. */
   for (uint32_t ref = AIMEE_PLUGIN_REF_FIRST; ref < AIMEE_PLUGIN_REF_LIMIT; ref++)
   {
      uint32_t invoke_kind = AIMEE_PLUGIN_KIND(ref, AIMEE_PLUGIN_STAGE_INVOKE);
      uint32_t declare_kind = AIMEE_PLUGIN_KIND(ref, AIMEE_PLUGIN_STAGE_DECLARE);
      if (!obs_bus_module_available(declare_kind))
         continue;
      char module_id[64];
      snprintf(module_id, sizeof module_id, "plugin:%u", ref);

      aimee_plugin_state_t state = AIMEE_PLUGIN_STATE_ERROR;
      char err[128] = "";
      int n = collect_one(declare_kind, invoke_kind, module_id, &state, err, sizeof err);

      /* The group is whatever this instance's commands landed under; read it
       * back from the registry rather than tracking it separately. */
      const char *group = "";
      for (size_t i = 0; i < g_owned_count; i++)
      {
         if (g_owned[i]->module && strcmp(g_owned[i]->module, module_id) == 0)
         {
            group = g_owned[i]->group ? g_owned[i]->group : "";
            break;
         }
      }
      status_record(ref, state, n > 0 ? n : 0, group, err);

      if (n < 0)
         continue;
      g_plugin_count++;
      total += n;
   }

   if (total)
      LOG_INFO("commands", "registered %d plugin command(s) from %zu instance(s)", total,
               g_plugin_count);
   return total;
}

/* Monotonic milliseconds, or 0 when the clock is unreadable. A zero reading
 * makes refresh() always collect, which is the safe direction: a stale command
 * list is invisible to the operator, an extra bus call is merely cost. */
static uint64_t now_ms(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t g_last_collect_ms;
static pthread_mutex_t g_collect_lock = PTHREAD_MUTEX_INITIALIZER;

int aimee_module_commands_refresh(int ttl_ms)
{
   if (ttl_ms < 0)
      ttl_ms = 0;
   pthread_mutex_lock(&g_collect_lock);
   uint64_t now = now_ms();
   if (now != 0 && g_last_collect_ms != 0 && now - g_last_collect_ms < (uint64_t)ttl_ms)
   {
      pthread_mutex_unlock(&g_collect_lock);
      return 0;
   }
   int registered = aimee_module_commands_collect();
   g_last_collect_ms = now;
   pthread_mutex_unlock(&g_collect_lock);
   return registered;
}

void aimee_module_commands_reset(void)
{
   pthread_mutex_lock(&g_collect_lock);
   g_last_collect_ms = 0;
   pthread_mutex_unlock(&g_collect_lock);
   for (size_t i = 0; i < g_owned_count; i++)
   {
      if (g_owned[i]->module)
         aimee_command_unregister_module(g_owned[i]->module);
   }
   for (size_t i = 0; i < g_owned_count; i++)
      owned_free_one(g_owned[i]);
   g_status_count = 0;
   free(g_owned);
   g_owned = NULL;
   g_owned_count = 0;
   g_owned_cap = 0;
   g_plugin_count = 0;
}

size_t aimee_module_commands_plugin_count(void)
{
   return g_plugin_count;
}
