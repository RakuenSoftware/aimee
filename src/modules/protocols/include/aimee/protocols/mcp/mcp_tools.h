#ifndef DEC_MCP_TOOLS_H
#define DEC_MCP_TOOLS_H 1

#include "cJSON.h"
#include <stddef.h>

/* Build one MCP tool descriptor object: {name, description, inputSchema:schema}.
 * Takes ownership of `schema` (added to the returned object). The shared builder
 * for every mcp_*_tools.c source — the tool wire shape lives here, once. Inline
 * so a minimal TU (e.g. mcp_tools_gateway.o in a unit test) needs no extra link
 * dependency, matching the json_fluent.h leaf-helper pattern. */
static inline cJSON *mcp_tool_new(const char *name, const char *desc, cJSON *schema)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON_AddItemToObject(t, "inputSchema", schema);
   return t;
}

/* Build the complete MCP tools list (core + git tools).
 * Returns a cJSON array suitable for tools/list responses. */
cJSON *mcp_build_tools_list(void);
/* The same list with coherent families left FLAT (uncollapsed). The collapsed form
 * is a presentation choice for external clients — one `index` tool with a command
 * discriminator instead of N. aimee's own agents need the individual names, because
 * a toolset grants tools one at a time. See mcp_collapse_families. */
cJSON *mcp_build_tools_list_flat(void);

/* Resolve the active MCP presentation profile: the explicit argument if set,
 * else AIMEE_MCP_TOOL_PROFILE, else "core" (the lean default). Not owned. */
const char *mcp_tool_profile_effective(const char *explicit_profile);

/* Filter a served tools/list IN PLACE to the named profile (NULL => resolve via
 * mcp_tool_profile_effective). "full" / unknown is a no-op for the PROFILE (fail
 * open); "core" or "lean" keeps only the Tier-0 set. Returns tools removed.
 *
 * delegates_enabled == 0 additionally removes the delegate tools, on EVERY
 * profile including "full": a profile decides how much of a working surface to
 * show, whereas delegation being off decides what exists at all. Passed in
 * rather than read from config because this module may not reach the config
 * module directly; the server supplies it. Pass 1 to keep them. */
int mcp_filter_tools_for_profile(cJSON *tools, const char *profile, int delegates_enabled);

/* Byte caps for the trimmed presentation. A top-level description keeps roughly a
 * sentence. Parameter hints are dropped outright (cap 0): they were 7,841 of the
 * surface's 11,894 prose bytes -- the largest single block -- and the type, enum and
 * required list beside each one already carry everything needed to construct a call.
 * describe_tool still returns the full text for a caller that wants the guidance. */
#define MCP_TOOL_PROSE_TOP_CAP   180
#define MCP_TOOL_PROSE_PARAM_CAP 0

/* Shorten guidance prose in a tools/list payload, in place. Hides no tool and alters
 * no callable shape -- types, enums and required lists are untouched; describe_tool
 * still returns the full text. Returns the number of tools visited.
 *
 * Opt-in via AIMEE_MCP_TOOL_PROSE=lean (default "full" leaves the payload as-is), so
 * the two presentations can be measured against each other rather than swapped on a
 * hunch. */
int mcp_compact_tool_prose(cJSON *tools);
int mcp_tool_prose_lean(void);

/* Append the find_tools / describe_tool discovery meta-tools and the call_tool
 * bridge to a tools list. Called by mcp_build_tools_list so they are always
 * present (and in the core profile), keeping a lean presentation usable from
 * schema-bound MCP hosts. */
void mcp_add_discovery_tools(cJSON *tools);

/* Case-insensitive discovery matcher shared by find_tools and its contract
 * tests. Empty queries match every descriptor. */
int mcp_tool_matches_query(const cJSON *tool, const char *query);

/* Resolve agent-facing code-search scope from MCP arguments. `project` wins;
 * otherwise cwd's basename is the active indexed project id. The scope helper
 * returns 0 for current/default, 1 for all, and -1 for invalid input. */
const char *mcp_code_project_from_args(cJSON *args);
int mcp_code_scope_all(cJSON *args);

/* Resolve call_tool({name,arguments}) to the named tool and its argument object.
 * MCP hosts can only invoke schemas returned by tools/list, so this bridge is
 * what makes tools found through find_tools genuinely callable under the lean
 * presentation profile. Returns 1 (rewritten), 0 (not call_tool), or -1 (bad
 * wrapper arguments / recursive target). The returned pointers borrow args. */
int mcp_call_tool_demux(const char *tool, cJSON *args, const char **out_tool, cJSON **out_args);

/* Append the P3 extended read-only tools (roadmap/task/index/memory-explain) to
 * a tools list. Definitions live in mcp_tools_extended.c; the matching content
 * handlers live in server_mcp_call_table.inc. Called by mcp_build_tools_list. */
void mcp_add_extended_tools(cJSON *tools);

/* Collapse coherent tool families (pipeline/diagnose/session/lsp/note/…) IN
 * PLACE: each family's member tools are merged into one tool with a command/
 * action discriminator. Called by mcp_build_tools_list after all members exist. */
void mcp_collapse_families(cJSON *tools);

/* If `tool` is a collapsed family name, resolve args[command|action] to the
 * legacy member tool name into `out`. Returns 1 (rewritten), 0 (not a family),
 * or -1 (family but missing/unknown command). */
int mcp_family_demux(const char *tool, cJSON *args, char *out, size_t n);

#endif /* DEC_MCP_TOOLS_H */
