/* module_commands.h: collect command declarations from the module bus.
 * Fixed modules opt into common discovery stage 255 (DCMD/DCMR version 2),
 * declaring their invocation stage and public commands. Plugin instances use
 * their existing stage-2 declaration/admission protocol (version 1).
 *
 * The host owns framing, registration lifetime, and route snapshots. Each
 * module owns its argument validation, behavior, and response construction. */
#ifndef DEC_MODULE_COMMANDS_H
#define DEC_MODULE_COMMANDS_H 1

#include <stddef.h>
#include <stdint.h>

/* This header declares a cJSON * parameter, so it carries the type
 * rather than relying on every includer having pulled it in first. */
#include "cJSON.h"

/* Principal refs reserved for plugin instances. MUST match
 * PluginRefFirst/PluginRefLimit in server-go/modules/mcp/mcp.go, and the
 * reservation recorded in tests/baselines/modules/canonical-inventory.yaml. */
#define AIMEE_PLUGIN_REF_FIRST 200u
#define AIMEE_PLUGIN_REF_LIMIT 456u

/* Stage ids inside a plugin module. MUST match server-go/modules/mcp/mcp.go. */
#define AIMEE_PLUGIN_STAGE_INVOKE  1u
#define AIMEE_PLUGIN_STAGE_DECLARE 2u

/* Optional common discovery stage for fixed modules (DCMD/DCMR version 2). */
#define AIMEE_MODULE_STAGE_DESCRIBE_COMMANDS 255u

/* A plugin instance's event kinds are derived from its principal ref by the
 * canonical module rule, 4096 + ref*256 + stage (docs/modules/README.md). The
 * ref is the single allocation authority; nothing else picks a kind.
 *
 * Plugin instances previously drew from a separate range at 11264, which is
 * exactly postgres's block (4096 + 28*256) and overlapped db2 and db1 as well.
 * Deriving from the ref makes that class of collision impossible. */
#define AIMEE_PLUGIN_KIND(ref, stage) (4096u + (uint32_t)(ref) * 256u + (uint32_t)(stage))

/* Invoke a command through the shared CMPQ/CMPS contract. Fixed modules and
 * plugin modules use the same framing; this layer never interprets arguments.
 * The caller owns the returned JSON object. NULL denotes a transport failure. */
cJSON *aimee_module_command_call(uint32_t event_kind, uint32_t stage_id, const char *verb,
                                 const cJSON *args);

/* Ask every attached declaring module for its commands and register them.
 *
 * Idempotent by construction: each declarant's previous commands are withdrawn
 * before its fresh set is registered, so calling this again after a plugin's
 * tool set changed converges rather than colliding on duplicates. Returns the
 * number of commands registered across all modules, or -1 if the bus is not
 * usable. A module that is absent, refuses, or answers malformed is skipped with
 * a log line -- one bad module must not cost every other module its commands. */
int aimee_module_commands_collect(void);

/* Refresh declarations and dispatch a public RPC from a copied route. Returns
 * 0 if undeclared, 1 with an owned JSON result, -1 for a transport failure. */
int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result);
/* Host-only fixed-module commands (zero declared surfaces). These are absent
 * from CLI/RPC/MCP/ACP discovery and cannot be invoked by public dispatch. */
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result);
/* Same host-only route, with a positive invocation budget in milliseconds.
 * Discovery and route copying retain their normal behavior. */
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result);
/* The host may attach verified request context to the same internal fixed-module
 * route. This does not publish the command or permit plugin context claims. */
int aimee_module_commands_dispatch_internal_context_timeout(const char *method, const cJSON *args,
                                                            const cJSON *context, int timeout_ms,
                                                            cJSON **result);
/* Verified request context stays separate from untrusted command arguments.
 * Only fixed modules receive the v2 frame; plugin invocation remains v1. */
int aimee_module_commands_dispatch_context(const char *method, const cJSON *args,
                                           const cJSON *context, cJSON **result);
/* For serialization-only callers: validate complete JSON and retain exact
 * numeric tokens. The owned raw node may be printed/copied, not inspected. */
int aimee_module_commands_dispatch_raw_context(const char *method, const cJSON *args,
                                               const cJSON *context, cJSON **result);
cJSON *aimee_module_command_call_context(uint32_t event_kind, uint32_t stage_id, const char *verb,
                                         const cJSON *args, const cJSON *context);

/* Collect again only if the last collect is older than ttl_ms.
 *
 * Plugin modules attach when THEY start, not when the daemon does, so a
 * boot-only collect would see an empty bus and advertise nothing for a plugin
 * that came up a second later. Surfaces that build a command view call this
 * instead, so an instance appears without a daemon restart. The TTL is what
 * keeps that from costing a bus round trip per attached instance on every
 * request. Returns the number of commands registered by the collect it ran, or
 * 0 when it was still fresh. */
int aimee_module_commands_refresh(int ttl_ms);

/* Withdraw and free everything this file registered. For daemon shutdown and
 * for tests. */
void aimee_module_commands_reset(void);

/* Number of plugin instances that answered the last collect. Diagnostics. */
size_t aimee_module_commands_plugin_count(void);

/* What one plugin instance is doing, for the operator surface.
 *
 * "Is my plugin running, and if not why not" is otherwise unanswerable from
 * outside: a module that is attached but refused by the OSV gate, one whose
 * plugin exited, and one that was never configured all present identically as
 * zero commands. These states have to be distinguishable or the only debugging
 * tool is the daemon log. */
typedef enum
{
   AIMEE_PLUGIN_STATE_ABSENT = 0, /* nothing serving this ref's declare kind */
   AIMEE_PLUGIN_STATE_PENDING,    /* attached, waiting on an admission verdict */
   AIMEE_PLUGIN_STATE_REFUSED,    /* the supply-chain gate blocked its plugin */
   AIMEE_PLUGIN_STATE_ACTIVE,     /* admitted and declaring commands */
   AIMEE_PLUGIN_STATE_SILENT,     /* admitted but declaring nothing (plugin gone?) */
   AIMEE_PLUGIN_STATE_ERROR       /* attached but the last exchange failed */
} aimee_plugin_state_t;

typedef struct
{
   uint32_t principal_ref;
   int command_count;
   aimee_plugin_state_t state;
   char group[64];       /* registry group its commands land under, "" if none */
   char last_error[128]; /* empty when the last exchange succeeded */
} aimee_plugin_status_t;

const char *aimee_plugin_state_name(aimee_plugin_state_t state);

/* Fill `out` with up to `max` attached instances; returns how many were written.
 * Reports what the LAST collect observed rather than probing the bus, so an
 * operator surface cannot stall behind a wedged plugin. */
int aimee_module_commands_snapshot(aimee_plugin_status_t *out, int max);

/* Serialise the plugin-instance table into a response object. */
void aimee_module_commands_report(cJSON *resp);

#endif /* DEC_MODULE_COMMANDS_H */
