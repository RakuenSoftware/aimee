/* command_registry.h: THE command table. One registration, every surface.
 *
 * A capability is registered ONCE, here, by the module that owns it. CLI, the v1
 * RPC routes, MCP and ACP all route from this table. If a command is not in it,
 * it is not reachable from any surface -- that is the point, and it is the
 * property that did not hold before.
 *
 * WHAT THIS REPLACES, and why it exists. Capability surface used to be declared
 * four independent times, by hand, in four files:
 *
 *   cli_v1_routes.c      cli_command_routes[] 216 entries  (CLI group+verb -> method)
 *   server_mcp_call_table.c mcp_tool_table[]   77 entries  (MCP dispatch)
 *   mcp_tool_profile.c   MCP_CORE_TOOLS[]      19 entries  (what tools/list shows)
 *   acp_server.c         its own handling
 *
 * Nothing derived one from another and nothing asserted they agreed, so they did
 * not. Measured consequences, all found on 2026-08-11: `memory_get` reachable as
 * `aimee memory get` and as an MCP tool but absent from the surface an agent is
 * shown; `get_context_block` marked native="core,..." in one table while missing
 * from the OTHER list called core; and the standing agent guidance naming three
 * tools no external client could call, which is why agents fell back to shell.
 *
 * THE INVARIANT: registration is the only way in. A surface enumerates the
 * registry; it does not keep a list of its own. Adding a command to one surface
 * and not another stops being expressible rather than being a thing we check for.
 */
#ifndef DEC_COMMAND_REGISTRY_H
#define DEC_COMMAND_REGISTRY_H 1

#include <stddef.h>

struct cJSON;

/* Which surfaces a command is exposed on. CLI is the capability floor: anything
 * exposed through MCP must also be callable from the CLI. The mask still permits
 * CLI/RPC-only commands, but there is deliberately no valid MCP-only shape. */
typedef enum
{
   AIMEE_SURFACE_CLI = 1u << 0, /* `aimee <group> <verb>` */
   AIMEE_SURFACE_RPC = 1u << 1, /* v1 route, "<group>.<verb>" */
   AIMEE_SURFACE_MCP = 1u << 2, /* MCP tool */
   AIMEE_SURFACE_ACP = 1u << 3, /* ACP */
   AIMEE_SURFACE_ALL = 0xFu,
} aimee_surface_t;

/* Whether an MCP client sees the command in tools/list, or must discover it.
 *
 * PROMINENT is the floor every lean client is shown. It is deliberately small --
 * the upfront payload is a per-session tax on every client -- but the cost of
 * getting it wrong is not "one extra call": mcp_tool_profile.c records the
 * measurement, that agents handed a tool costing find_tools -> describe_tool ->
 * call_tool used a recursive text search instead. A tool the agent cannot afford
 * to reach is a tool it does not have. */
typedef enum
{
   AIMEE_MCP_PROMINENT = 0, /* listed in tools/list */
   AIMEE_MCP_DISCOVERABLE,  /* reachable via find_tools/describe_tool/call_tool */
} aimee_mcp_visibility_t;

/* A command handler. `args` is the argument object for this invocation, shaped by
 * the command's own schema, whichever surface it arrived on -- translating the
 * wire form into it is the surface's job, not the handler's. Returns a malloc'd
 * cJSON result the caller owns, or NULL on failure. */
typedef struct cJSON *(*aimee_command_fn)(const struct cJSON *args, void *ud);

typedef struct
{
   const char *group; /* "memory" */
   const char *verb;  /* "get"  -> CLI `aimee memory get`, RPC "memory.get" */
   const char *summary;
   struct cJSON *schema; /* argument schema; borrowed, owned by the registrant */
   unsigned surfaces;    /* aimee_surface_t mask */
   aimee_mcp_visibility_t mcp_visibility;
   aimee_command_fn fn;
   void *ud;
   const char *module; /* registering module id, for diagnostics */
} aimee_command_t;

/* Register one command. Returns 0 on success, -1 on a duplicate (group, verb),
 * malformed entry, or any MCP exposure without CLI exposure.
 * Duplicate registration is an ERROR rather than a silent overwrite: two
 * modules claiming one name is a build-order-dependent bug, and the whole
 * purpose of this table is that a name resolves one way. */
int aimee_command_register(const aimee_command_t *cmd);

/* Register a contiguous array in one call, for a module declaring its set at bus
 * connect. Stops and returns -1 at the first failure, having registered the
 * entries before it -- the caller is expected to treat that as fatal. */
int aimee_command_register_many(const aimee_command_t *cmds, size_t n);

/* Drop every command registered by `module`, returning how many were removed.
 *
 * Registration is otherwise append-only, which was right while every registrant
 * was a compiled-in module whose command set could not change. A PLUGIN module
 * can: its commands are whatever the plugin it hosts advertises, so a plugin
 * that disconnects has to take its commands with it. Leaving them registered
 * would advertise capabilities to every surface that answer only "unavailable",
 * and would block the same plugin re-registering on reconnect, since a duplicate
 * (group, verb) is refused.
 *
 * INVALIDATES pointers previously returned by aimee_command_find/_at: entries
 * after the removed ones shift down. Every surface enumerates synchronously and
 * does not hold one across a mutation, which is the property this relies on. */
size_t aimee_command_unregister_module(const char *module);

/* Bumped on every registration, withdrawal, and reset.
 *
 * A surface that caches a view of the table (an MCP tools/list, a CLI manifest)
 * needs to know the table changed WITHOUT diffing it. It is also what makes a
 * push notification possible at all: MCP advertises tools.listChanged, and a
 * client that trusts that capability and is never notified simply never re-lists.
 * Monotonic within a process; not meaningful across restarts. */
unsigned long aimee_command_registry_generation(void);

/* Lookup. NULL when absent -- which is the same answer every surface must give,
 * so an unregistered command is unroutable everywhere alike. */
const aimee_command_t *aimee_command_find(const char *group, const char *verb);

/* Lookup by the RPC/dotted spelling, "<group>.<verb>". */
const aimee_command_t *aimee_command_find_method(const char *method);

/* Enumeration, for a surface building its own view (tools/list, CLI help, the v1
 * route table). Index order is registration order. */
size_t aimee_command_count(void);
const aimee_command_t *aimee_command_at(size_t index);

/* Register a capability supplied by an external Runtime/Control-Plane module
 * that genuinely has only one transport. This is deliberately separate from
 * the command table: internal commands must keep CLI as their capability floor,
 * while a future module may expose only MCP (or only CLI) because the other
 * transport cannot be registered in the client. `surface` must be exactly one
 * of AIMEE_SURFACE_CLI or AIMEE_SURFACE_MCP. Strings remain owned by the caller,
 * like aimee_command_t registration. */
int aimee_agent_surface_register(const char *name, unsigned surface, const char *module);

/* Build the agent-facing one-surface projection from CLI-only commands plus
 * explicitly registered one-surface external module capabilities:
 * {"cli_only":[...],"mcp_only":[...]}. The caller owns the cJSON object. */
struct cJSON *aimee_command_agent_surfaces_json(void);

/* Test/teardown only: drop every command and external-surface registration. */
void aimee_command_registry_reset(void);

#endif /* DEC_COMMAND_REGISTRY_H */
