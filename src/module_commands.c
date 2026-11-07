/* module_commands.c: see headers/module_commands.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "headers/module_commands.h"
#include <ouroboros/module_protocol.h>

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

extern int aimee_module_owner_action_request(const char *, const cJSON *, int, char **)
    __attribute__((weak));
int aimee_module_commands_owner_action_raw(const char *method, const cJSON *args, int timeout_ms,
                                           char **result)
{
   if (!result)
      return -1;
   *result = NULL;
   if (!method || !method[0] || !cJSON_IsObject(args) || timeout_ms <= 0)
      return -1;
   if (!aimee_module_owner_action_request)
      return 0;
   char *raw = NULL;
   int selected = aimee_module_owner_action_request(method, args, timeout_ms, &raw);
   if (selected != 1)
   {
      int invalid = raw != NULL;
      free(raw);
      return selected == 0 && !invalid ? 0 : -1;
   }
   cJSON *parsed =
       raw && strnlen(raw, OURO_MODULE_MESSAGE_MAX_BODY + 1u) <= OURO_MODULE_MESSAGE_MAX_BODY
           ? cJSON_ParseWithOpts(raw, NULL, 1)
           : NULL;
   int valid = cJSON_IsObject(parsed);
   cJSON_Delete(parsed);
   if (!valid)
   {
      free(raw);
      return -1;
   }
   *result = raw; /* Validate without normalizing native integer tokens. */
   return 1;
}

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
#define INVOKE_MAX_RESPONSE    OURO_MODULE_MESSAGE_MAX_BODY
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
   char *module; /* registry `module` field, also the withdrawal key */
   int internal; /* fixed-module route, absent from every external surface */
   uint32_t invoke_kind;
   uint32_t invoke_stage;
   char verb_wire[128]; /* verb as sent to the module, for the invoke frame */
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

/* --- shared command dispatch ------------------------------------------------ */

/* Encodes the shared invoke frame and returns the owner's JSON result.
 * Fixed modules use their declared stage; plugins retain their stage-1 route. Returns NULL on any
 * failure; the surface that called decides what that means, exactly as module_json_call.c does. */
typedef struct
{
   char *args_json;
   char *context_json;
   unsigned char *request;
   unsigned char *response;
   char *body;
   cJSON *parsed;
} command_call_scope_t;
static void command_call_cleanup(void *opaque)
{
   volatile command_call_scope_t *scope = opaque;
   free(scope->args_json);
   free(scope->context_json);
   free(scope->request);
   free(scope->response);
   free(scope->body);
   cJSON_Delete(scope->parsed);
}
static void command_outcome_reset(aimee_module_command_outcome_v1 *outcome)
{
   if (outcome)
      *outcome = (aimee_module_command_outcome_v1){1, AIMEE_COMMAND_UNSENT, -1};
}
__attribute__((used, externally_visible, retain))
const unsigned aimee_module_command_outcome_contract_v1 = 1;
static cJSON *command_call_timeout_outcome(uint32_t event_kind, uint32_t stage_id, const char *verb,
                                           const cJSON *args, const cJSON *context, int timeout_ms,
                                           int raw_result, aimee_module_command_outcome_v1 *outcome)
{
   command_outcome_reset(outcome);
   if (!event_kind || !stage_id || !verb || !verb[0] || strlen(verb) > 127)
      return NULL;
   volatile command_call_scope_t owned = {0};
   cJSON *volatile result = NULL;
   pthread_cleanup_push(command_call_cleanup, (void *)&owned);
   size_t args_len = 0;
   if (args)
   {
      owned.args_json = cJSON_PrintUnformatted((cJSON *)args);
      if (!owned.args_json)
         goto done;
      args_len = strlen(owned.args_json);
   }
   owned.context_json = context && cJSON_IsObject(context) ? cJSON_PrintUnformatted(context) : NULL;
   if (context && !owned.context_json)
      goto done;
   size_t context_len = owned.context_json ? strlen(owned.context_json) : 0;
   size_t header_len = context ? 20 : INVOKE_REQUEST_HEADER;
   size_t verb_len = strlen(verb);
   size_t request_len = header_len + verb_len + args_len + context_len;
   if (request_len > OURO_MODULE_MESSAGE_MAX_BODY || context_len > 4096)
      goto done;
   owned.request = malloc(request_len);
   if (!owned.request)
      goto done;
   wr_u32(owned.request, INVOKE_REQUEST_MAGIC);
   wr_u32(owned.request + 4, context ? 2 : DECL_WIRE_VERSION);
   wr_u16(owned.request + 8, (uint16_t)verb_len);
   wr_u16(owned.request + 10, 0);
   wr_u32(owned.request + 12, (uint32_t)args_len);
   if (context)
      wr_u32(owned.request + 16, (uint32_t)context_len);
   memcpy(owned.request + header_len, verb, verb_len);
   if (args_len)
      memcpy(owned.request + header_len + verb_len, owned.args_json, args_len);
   if (context_len)
      memcpy(owned.request + header_len + verb_len + args_len, owned.context_json, context_len);
   free(owned.context_json);
   owned.context_json = NULL;
   free(owned.args_json);
   owned.args_json = NULL;
   owned.response = malloc(INVOKE_MAX_RESPONSE);
   if (!owned.response)
      goto done;
   uint32_t response_len = 0;
   ouro_module_call_result_t rc = obs_bus_module_call(
       event_kind, stage_id, 0, aimee_module_call_deadline_ns(timeout_ms), owned.request,
       (uint32_t)request_len, owned.response, INVOKE_MAX_RESPONSE, &response_len, NULL, NULL);
   free(owned.request);
   owned.request = NULL;
   if (outcome)
   {
      outcome->stage = AIMEE_COMMAND_SDK_RESULT;
      outcome->sdk_result = (int)rc;
   }
   if (rc != OURO_MODULE_CALL_OK)
   {
      LOG_WARN("commands", "command %s: invoke failed: %s", verb, ouro_module_call_result_name(rc));
      goto done;
   }
   if (outcome)
      outcome->stage = AIMEE_COMMAND_FRAME_INVALID;
   if (response_len < INVOKE_RESPONSE_HEADER || rd_u32(owned.response) != INVOKE_RESPONSE_MAGIC ||
       rd_u32(owned.response + 4) != DECL_WIRE_VERSION)
   {
      LOG_WARN("commands", "command %s: malformed frame", verb);
      goto done;
   }
   uint32_t body_len = rd_u32(owned.response + 8);
   if ((uint64_t)INVOKE_RESPONSE_HEADER + body_len != response_len)
   {
      LOG_WARN("commands", "command %s: frame length disagrees with its header", verb);
      goto done;
   }
   /* The raw sibling transports untrusted response bytes without normalizing
    * JSON tokens. Its caller must validate the complete body before use. */
   if (outcome)
      outcome->stage = AIMEE_COMMAND_LOCAL_FAILURE;
   owned.body = malloc((size_t)body_len + 1);
   if (!owned.body)
      goto done;
   memcpy(owned.body, owned.response + INVOKE_RESPONSE_HEADER, body_len);
   owned.body[body_len] = '\0';
   free(owned.response);
   owned.response = NULL;
   if (outcome)
      outcome->stage = AIMEE_COMMAND_BODY_INVALID;
   if (raw_result == 2)
   {
      /* A cJSON Raw node owns a C string. Reject literal NUL bytes rather than
       * truncate; escaped JSON NUL remains exact bytes for the receiver parser. */
      if (!memchr(owned.body, '\0', body_len))
         owned.parsed = cJSON_CreateRaw(owned.body);
   }
   else
   {
      const char *end = NULL;
      owned.parsed = cJSON_ParseWithLengthOpts(owned.body, (size_t)body_len + 1, &end, 1);
      if (owned.parsed && end != owned.body + body_len)
      {
         cJSON_Delete(owned.parsed);
         owned.parsed = NULL;
      }
      if (owned.parsed && raw_result == 1)
      {
         cJSON_Delete(owned.parsed);
         owned.parsed = cJSON_CreateRaw(owned.body);
      }
   }
   result = owned.parsed;
   owned.parsed = NULL;
   if (outcome && result)
      outcome->stage = AIMEE_COMMAND_BODY_READY;
done:
   pthread_cleanup_pop(1);
   return result;
}

static cJSON *command_call_timeout(uint32_t event_kind, uint32_t stage_id, const char *verb,
                                   const cJSON *args, const cJSON *context, int timeout_ms,
                                   int raw_result)
{
   return command_call_timeout_outcome(event_kind, stage_id, verb, args, context, timeout_ms,
                                       raw_result, NULL);
}

cJSON *aimee_module_command_call_context_timeout(uint32_t event_kind, uint32_t stage_id,
                                                 const char *verb, const cJSON *args,
                                                 const cJSON *context, int timeout_ms)
{
   return timeout_ms > 0
              ? command_call_timeout(event_kind, stage_id, verb, args, context, timeout_ms, 0)
              : NULL;
}

__attribute__((used, externally_visible, retain)) cJSON *
aimee_module_command_call_raw_context_timeout(uint32_t event_kind, uint32_t stage_id,
                                              const char *verb, const cJSON *args,
                                              const cJSON *context, int timeout_ms)
{
   return timeout_ms > 0
              ? command_call_timeout(event_kind, stage_id, verb, args, context, timeout_ms, 2)
              : NULL;
}

cJSON *aimee_module_command_call_context(uint32_t event_kind, uint32_t stage_id, const char *verb,
                                         const cJSON *args, const cJSON *context)
{
   return command_call_timeout(event_kind, stage_id, verb, args, context, INVOKE_TIMEOUT_MS, 0);
}

cJSON *aimee_module_command_call(uint32_t event_kind, uint32_t stage_id, const char *verb,
                                 const cJSON *args)
{
   return aimee_module_command_call_context(event_kind, stage_id, verb, args, NULL);
}

static cJSON *plugin_command_invoke(const cJSON *args, void *ud)
{
   const owned_command_t *cmd = ud;
   if (!cmd)
      return NULL;
   return aimee_module_command_call(cmd->invoke_kind, cmd->invoke_stage, cmd->verb_wire, args);
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
   ouro_module_call_result_t rc = obs_bus_module_call(
       declare_kind, AIMEE_PLUGIN_STAGE_DECLARE, 0, aimee_module_call_deadline_ns(60000), request,
       sizeof request, ack, DECL_MAX_RESPONSE, &out_len, NULL, NULL);
   free(ack);
   if (rc != OURO_MODULE_CALL_OK)
   {
      LOG_WARN("commands", "module %s: admission verdict not delivered: %s", module_id,
               ouro_module_call_result_name(rc));
      return 0;
   }
   LOG_INFO("commands", "module %s: plugin %s by the OSV gate", module_id,
            blocked ? "REFUSED" : "admitted");
   return 1;
}

/* Weak so unit binaries that stub the bus without it link; with no fingerprint
 * the registry falls back to its time-to-live alone. */
uint64_t obs_bus_slot_fingerprint(void) __attribute__((weak));

static uint64_t g_last_collect_ms;
/* The bus's slot fingerprint when the registry was last collected, and whether
 * some module failed to answer then (so the next refresh asks again). */
static uint64_t g_last_collect_fingerprint;
static int g_collect_incomplete;

static int collect_one(uint32_t declare_kind, uint32_t declare_stage, uint32_t invoke_kind,
                       uint32_t version, const char *module_id, aimee_plugin_state_t *state_out,
                       char *err_out, size_t err_cap)
{
   int refused = 0;
   if (state_out)
      *state_out = AIMEE_PLUGIN_STATE_ERROR;
   unsigned char request[8];
   wr_u32(request, DECL_REQUEST_MAGIC);
   wr_u32(request + 4, version);

   unsigned char *response = malloc(DECL_MAX_RESPONSE);
   if (!response)
      return -1;
   uint32_t response_len = 0;

   /* Up to two rounds: the first may come back asking for an admission verdict,
    * and the second is the declaration that follows it. One retry only -- a
    * module that keeps asking is misbehaving, and looping would spin the gate. */
   int rounds = 0;
   ouro_module_call_result_t rc;
   for (;;)
   {
      response_len = 0;
      rc = obs_bus_module_call(declare_kind, declare_stage, 0, aimee_module_call_deadline_ns(5000),
                               request, sizeof request, response, DECL_MAX_RESPONSE, &response_len,
                               NULL, NULL);
      if (rc != OURO_MODULE_CALL_OK)
      {
         g_collect_incomplete = 1; /* ask this module again on the next refresh */
         LOG_WARN("commands", "module %s declaration failed: %s", module_id,
                  ouro_module_call_result_name(rc));
         if (err_out)
            snprintf(err_out, err_cap, "declaration failed: %s", ouro_module_call_result_name(rc));
         free(response);
         return -1;
      }
      if (version == 1 && response_len >= DECL_HEADER_LEN &&
          rd_u32(response) == DECL_PENDING_MAGIC && rd_u32(response + 4) == DECL_WIRE_VERSION &&
          rounds == 0)
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
       rd_u32(response + 4) != version)
   {
      LOG_WARN("commands", "module %s sent a malformed declaration header", module_id);
      if (err_out)
         snprintf(err_out, err_cap, "malformed declaration header");
      free(response);
      return -1;
   }
   uint32_t count = rd_u32(response + 8);
   uint32_t header_len = DECL_HEADER_LEN;
   uint32_t invoke_stage = AIMEE_PLUGIN_STAGE_INVOKE;
   if (version == 2)
   {
      header_len = 16;
      if (response_len < header_len || count > 4096)
      {
         free(response);
         return -1;
      }
      invoke_stage = rd_u32(response + 12);
      if (!invoke_stage || invoke_stage >= AIMEE_MODULE_STAGE_DESCRIBE_COMMANDS)
      {
         free(response);
         return -1;
      }
      invoke_kind = declare_kind - declare_stage + invoke_stage;
      /* Refuse the complete declaration before changing the registry if any
       * record is malformed. A truncated owner table must not partly replace
       * a previously working public surface. */
      uint32_t cursor = header_len;
      for (uint32_t i = 0; i < count; ++i)
      {
         if ((uint64_t)cursor + DECL_RECORD_LEN > response_len)
            goto malformed_routes;
         uint32_t surfaces = rd_u32(response + cursor);
         uint32_t visibility = rd_u32(response + cursor + 4);
         uint16_t gl = rd_u16(response + cursor + 8);
         uint16_t vl = rd_u16(response + cursor + 10);
         uint16_t sl = rd_u16(response + cursor + 12);
         if ((surfaces & ~15u) || visibility > 1 || !gl || !vl || gl > 127 || vl > 127 ||
             rd_u16(response + cursor + 14) ||
             ((surfaces & AIMEE_SURFACE_MCP) && !(surfaces & AIMEE_SURFACE_CLI)) ||
             (uint64_t)cursor + DECL_RECORD_LEN + gl + vl + sl > response_len)
            goto malformed_routes;
         uint32_t previous = header_len;
         for (uint32_t j = 0; j < i; ++j)
         {
            uint16_t previous_gl = rd_u16(response + previous + 8);
            uint16_t previous_vl = rd_u16(response + previous + 10);
            uint16_t previous_sl = rd_u16(response + previous + 12);
            if (gl == previous_gl && vl == previous_vl &&
                memcmp(response + cursor + DECL_RECORD_LEN, response + previous + DECL_RECORD_LEN,
                       (size_t)gl + vl) == 0)
               goto malformed_routes;
            previous += DECL_RECORD_LEN + previous_gl + previous_vl + previous_sl;
         }
         cursor += DECL_RECORD_LEN;
         for (uint32_t j = 0; j < (uint32_t)gl + vl; ++j)
         {
            unsigned char c = response[cursor + j];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
               goto malformed_routes;
         }
         if (memchr(response + cursor, 0, (size_t)gl + vl + sl))
            goto malformed_routes;
         cursor += (uint32_t)gl + vl + sl;
      }
      if (cursor != response_len)
         goto malformed_routes;
   }

   /* Withdraw before re-registering: a duplicate (group, verb) is refused, so a
    * module whose tool set changed would otherwise never converge. */
   aimee_command_unregister_module(module_id);
   owned_release_module(module_id);

   int registered = 0;
   uint32_t off = header_len;
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
      owned->invoke_stage = invoke_stage;
      memcpy(owned->verb_wire, vp, vl);
      owned->verb_wire[vl] = '\0';
      if (!owned->group || !owned->verb || !owned->summary || !owned->module)
      {
         owned_pop_last();
         break;
      }

      owned->internal = version == 2 && surfaces == 0;
      if (owned->internal)
         continue;

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
   free(response);
   return registered;

malformed_routes:
   LOG_WARN("commands", "module %s sent malformed command routes", module_id);
   free(response);
   return -1;
}

/* --- public ----------------------------------------------------------------- */

int aimee_module_commands_collect(void)
{
   int total = 0;
   g_plugin_count = 0;
   g_status_count = 0;

   /* Fixed modules opt into one common discovery stage. Its reply carries the
    * invocation stage, so the host has no per-module command routing table. */
   for (uint32_t ref = 1; ref < AIMEE_PLUGIN_REF_FIRST; ++ref)
   {
      uint32_t kind = AIMEE_PLUGIN_KIND(ref, AIMEE_MODULE_STAGE_DESCRIBE_COMMANDS);
      char module_id[64];
      snprintf(module_id, sizeof module_id, "module:%u", ref);
      if (!obs_bus_module_available(kind))
      {
         aimee_command_unregister_module(module_id);
         owned_release_module(module_id);
         continue;
      }
      int n =
          collect_one(kind, AIMEE_MODULE_STAGE_DESCRIBE_COMMANDS, 0, 2, module_id, NULL, NULL, 0);
      if (n > 0)
         total += n;
   }

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
      int n = collect_one(declare_kind, AIMEE_PLUGIN_STAGE_DECLARE, invoke_kind, 1, module_id,
                          &state, err, sizeof err);

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
      LOG_INFO("commands", "registered %d module command(s), %zu plugin instance(s)", total,
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

static pthread_mutex_t g_collect_lock = PTHREAD_MUTEX_INITIALIZER;

int aimee_module_commands_refresh(int ttl_ms)
{
   if (ttl_ms < 0)
      ttl_ms = 0;
   pthread_mutex_lock(&g_collect_lock);
   uint64_t now = now_ms();
   /* What modules declare can only change when one attaches or leaves. While
    * that has not happened and the last collection heard from every module, the
    * registry is current: asking every module again -- which this used to do
    * every two seconds of tool traffic -- would only return the same answer.
    * ttl_ms still paces retries after a collection some module did not answer. */
   uint64_t fingerprint = obs_bus_slot_fingerprint ? obs_bus_slot_fingerprint() : 0;
   if (g_last_collect_ms != 0 && fingerprint != 0 && fingerprint == g_last_collect_fingerprint &&
       !g_collect_incomplete)
   {
      pthread_mutex_unlock(&g_collect_lock);
      return 0;
   }
   if (now != 0 && g_last_collect_ms != 0 && now - g_last_collect_ms < (uint64_t)ttl_ms)
   {
      pthread_mutex_unlock(&g_collect_lock);
      return 0;
   }
   g_collect_incomplete = 0;
   int registered = aimee_module_commands_collect();
   g_last_collect_ms = now;
   g_last_collect_fingerprint = fingerprint;
   pthread_mutex_unlock(&g_collect_lock);
   return registered;
}

static int commands_dispatch_context(const char *method, const cJSON *args, const cJSON *context,
                                     cJSON **result, int raw_result)
{
   if (!method || !result)
      return 0;
   *result = NULL;
   (void)aimee_module_commands_refresh(2000);
   uint32_t kind = 0, stage = 0;
   int fixed = 0;
   char verb[128] = "";
   pthread_mutex_lock(&g_collect_lock);
   const aimee_command_t *command = aimee_command_find_method(method);
   if (command && command->fn == plugin_command_invoke && (command->surfaces & AIMEE_SURFACE_RPC))
   {
      const owned_command_t *owned = command->ud;
      kind = owned->invoke_kind;
      stage = owned->invoke_stage;
      fixed = strncmp(owned->module, "module:", 7) == 0;
      snprintf(verb, sizeof verb, "%s", owned->verb_wire);
   }
   pthread_mutex_unlock(&g_collect_lock);
   if (!kind)
      return 0;
   /* A refresh may replace the registry while this call waits on the bus; only
    * the copied route crosses that wait, never a borrowed registry pointer. */
   *result = command_call_timeout(kind, stage, verb, args, fixed ? context : NULL,
                                  INVOKE_TIMEOUT_MS, raw_result);
   return *result ? 1 : -1;
}

int aimee_module_commands_dispatch_context(const char *method, const cJSON *args,
                                           const cJSON *context, cJSON **result)
{
   return commands_dispatch_context(method, args, context, result, 0);
}

int aimee_module_commands_dispatch_raw_context(const char *method, const cJSON *args,
                                               const cJSON *context, cJSON **result)
{
   return commands_dispatch_context(method, args, context, result, 1);
}

int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   return aimee_module_commands_dispatch_context(method, args, NULL, result);
}

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   return aimee_module_commands_dispatch_internal_timeout(method, args, 125000, result);
}

int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   return aimee_module_commands_dispatch_internal_context_timeout(method, args, NULL, timeout_ms,
                                                                  result);
}

/* Optional verifier-owned host context. Opt-in callers alone select v2 framing. */
extern cJSON *aimee_module_host_command_context(void) __attribute__((weak));

int aimee_module_commands_capture_caller_context(cJSON **context)
{
   if (!context)
      return -1;
   *context = NULL;
   if (!aimee_module_host_command_context)
      return 0;
   cJSON *value = aimee_module_host_command_context();
   const cJSON *auth = cJSON_GetObjectItemCaseSensitive(value, "authenticated");
   const cJSON *user = cJSON_GetObjectItemCaseSensitive(value, "user_authority");
   const cJSON *principal = cJSON_GetObjectItemCaseSensitive(value, "principal");
   const cJSON *transport = cJSON_GetObjectItemCaseSensitive(value, "transport_identity");
   const cJSON *kind = cJSON_GetObjectItemCaseSensitive(value, "scope_kind");
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(value, "scope_id");
   int scoped = kind || id;
   int valid = cJSON_IsObject(value) && cJSON_GetArraySize(value) == (scoped ? 6 : 4) &&
               cJSON_IsBool(auth) && cJSON_IsBool(user) && cJSON_IsString(principal) &&
               cJSON_IsString(transport);
   if (valid && cJSON_IsTrue(auth))
      valid = principal->valuestring[0] && transport->valuestring[0];
   if (valid && !cJSON_IsTrue(auth))
      valid = !cJSON_IsTrue(user) && !principal->valuestring[0] && !transport->valuestring[0] &&
              !scoped;
   if (valid && scoped)
      valid = cJSON_IsTrue(auth) && cJSON_IsString(kind) && kind->valuestring[0] &&
              cJSON_IsString(id) && id->valuestring[0];
   char *encoded = valid ? cJSON_PrintUnformatted(value) : NULL;
   valid = encoded && strlen(encoded) <= 4096;
   cJSON_free(encoded);
   if (!valid)
   {
      cJSON_Delete(value);
      return -1;
   }
   *context = value;
   return 1;
}

int aimee_module_commands_dispatch_internal_caller_timeout(const char *method, const cJSON *args,
                                                           int timeout_ms, cJSON **result)
{
   if (!result)
      return 0;
   *result = NULL;
   cJSON *context = NULL;
   if (aimee_module_commands_capture_caller_context(&context) < 0)
      return -1;
   int rc = aimee_module_commands_dispatch_internal_context_timeout(method, args, context,
                                                                    timeout_ms, result);
   cJSON_Delete(context);
   return rc;
}

int aimee_module_commands_dispatch_internal_caller(const char *method, const cJSON *args,
                                                   cJSON **result)
{
   return aimee_module_commands_dispatch_internal_caller_timeout(method, args, 125000, result);
}

static int commands_dispatch_internal_context_timeout_outcome(
    const char *method, const cJSON *args, const cJSON *context, int timeout_ms, cJSON **result,
    int raw_result, int declared_fixed, aimee_module_command_outcome_v1 *outcome)
{
   command_outcome_reset(outcome);
   if (!result)
      return 0;
   *result = NULL;
   if (!method || timeout_ms <= 0)
      return -1;
   (void)aimee_module_commands_refresh(2000);
   uint32_t kind = 0, stage = 0;
   char verb[128] = "";
   int matches = 0;
   pthread_mutex_lock(&g_collect_lock);
   for (size_t i = 0; i < g_owned_count; ++i)
   {
      const owned_command_t *owned = g_owned[i];
      if (declared_fixed ? (!owned->module || strncmp(owned->module, "module:", 7) != 0)
                         : !owned->internal)
         continue;
      size_t group_len = strlen(owned->group);
      if (strncmp(method, owned->group, group_len) != 0 || method[group_len] != '.' ||
          strcmp(method + group_len + 1, owned->verb) != 0)
         continue;
      ++matches;
      kind = owned->invoke_kind;
      stage = owned->invoke_stage;
      snprintf(verb, sizeof verb, "%s", owned->verb_wire);
   }
   pthread_mutex_unlock(&g_collect_lock);
   if (matches != 1 || !kind)
      return matches ? -1 : 0;
   *result = command_call_timeout_outcome(kind, stage, verb, args, context, timeout_ms, raw_result,
                                          outcome);
   return *result ? 1 : -1;
}

static int commands_dispatch_internal_context_timeout(const char *method, const cJSON *args,
                                                      const cJSON *context, int timeout_ms,
                                                      cJSON **result, int raw_result,
                                                      int declared_fixed)
{
   return commands_dispatch_internal_context_timeout_outcome(
       method, args, context, timeout_ms, result, raw_result, declared_fixed, NULL);
}
__attribute__((used, externally_visible, retain)) int
aimee_module_commands_dispatch_internal_raw_outcome_context_timeout_v1(
    const char *method, const cJSON *args, const cJSON *context, int timeout_ms, cJSON **result,
    aimee_module_command_outcome_v1 *outcome)
{
   if (!outcome)
      return -1;
   return commands_dispatch_internal_context_timeout_outcome(method, args, context, timeout_ms,
                                                             result, 1, 0, outcome);
}

int aimee_module_commands_dispatch_internal_context_timeout(const char *method, const cJSON *args,
                                                            const cJSON *context, int timeout_ms,
                                                            cJSON **result)
{
   return commands_dispatch_internal_context_timeout(method, args, context, timeout_ms, result, 0,
                                                     0);
}

int aimee_module_commands_dispatch_internal_raw_context_timeout(const char *method,
                                                                const cJSON *args,
                                                                const cJSON *context,
                                                                int timeout_ms, cJSON **result)
{
   return commands_dispatch_internal_context_timeout(method, args, context, timeout_ms, result, 1,
                                                     0);
}

__attribute__((used, externally_visible, retain)) int
aimee_module_commands_dispatch_declared_raw_context_timeout(const char *method, const cJSON *args,
                                                            const cJSON *context, int timeout_ms,
                                                            cJSON **result)
{
   return commands_dispatch_internal_context_timeout(method, args, context, timeout_ms, result, 1,
                                                     1);
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

/* Serialise the plugin-instance table into a response object.
 *
 * Lives here rather than in the endpoint because the snapshot is this module's:
 * the endpoint knows it wants plugin status, not how a status row is shaped. It
 * moved when the merge pushed server_state.c past the line limit, which is a
 * blunt reason for a change that was right anyway.
 */
void aimee_module_commands_report(cJSON *resp)
{
   if (!resp)
   {
      return;
   }
   /* Plugin modules: one row per attached instance.
    *
    * "Is my plugin running, and if not why not" is otherwise unanswerable from
    * outside the daemon log. An instance blocked by the supply-chain gate, one
    * whose plugin exited, and one that was never configured all present as zero
    * commands, and each needs a different action -- so the STATE is reported,
    * not just a count. Read off the last collect rather than probing, so this
    * endpoint cannot stall behind a wedged plugin. */
   {
      /* Refresh before reporting.
       *
       * Reading the snapshot alone made this endpoint useless as a diagnostic:
       * the status table is filled by a collect, and nothing here triggered one,
       * so "is my plugin running" answered "no plugins" until some OTHER surface
       * happened to ask. Found on the live host -- the instance was attached and
       * serving, and this said nothing. The TTL is what keeps the refresh from
       * costing a bus round trip per scrape. */
      aimee_module_commands_refresh(2000);

      aimee_plugin_status_t plugins[32];
      int n = aimee_module_commands_snapshot(plugins, 32);
      cJSON *arr = cJSON_AddArrayToObject(resp, "plugins");
      if (arr)
      {
         for (int i = 0; i < n; i++)
         {
            cJSON *row = cJSON_CreateObject();
            if (!row)
               continue;
            cJSON_AddNumberToObject(row, "principal_ref", (double)plugins[i].principal_ref);
            cJSON_AddStringToObject(row, "state", aimee_plugin_state_name(plugins[i].state));
            cJSON_AddStringToObject(row, "group", plugins[i].group);
            cJSON_AddNumberToObject(row, "commands", plugins[i].command_count);
            if (plugins[i].last_error[0])
               cJSON_AddStringToObject(row, "last_error", plugins[i].last_error);
            cJSON_AddItemToArray(arr, row);
         }
      }
   }
}
