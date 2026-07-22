#ifndef DEC_TOOLSET_H
#define DEC_TOOLSET_H 1

#include <stddef.h>

#define TOOLSET_NAME_MAX    64
#define TOOLSET_TOOL_MAX    128
#define TOOLSET_MAX_SETS    64
#define TOOLSET_MAX_INCLUDE 16
#define TOOLSET_MAX_TOOLS   128
#define TOOLSET_ERROR_MAX   256

typedef struct
{
   char name[TOOLSET_NAME_MAX];
   char include[TOOLSET_MAX_INCLUDE][TOOLSET_NAME_MAX];
   int include_count;
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   int tool_count;
   int builtin;
} toolset_def_t;

typedef struct
{
   toolset_def_t sets[TOOLSET_MAX_SETS];
   int count;
   char script_allowed_tools[TOOLSET_NAME_MAX];
} toolset_registry_t;

void toolset_registry_init(toolset_registry_t *registry);
int toolset_registry_load_effective(toolset_registry_t *registry, char *err, size_t err_len);
int toolset_registry_load_file(toolset_registry_t *registry, const char *path, char *err,
                               size_t err_len);
const toolset_def_t *toolset_registry_find(const toolset_registry_t *registry, const char *name);
int toolset_registry_validate(const toolset_registry_t *registry, char *err, size_t err_len);
int toolset_resolve(const toolset_registry_t *registry, const char *name,
                    char out[][TOOLSET_TOOL_MAX], int max_tools, char *err, size_t err_len);
int toolset_resolve_effective(const char *name, char out[][TOOLSET_TOOL_MAX], int max_tools,
                              char *err, size_t err_len);
int toolset_tool_known(const char *name);
const char *toolset_for_delegate_role(const char *role);

/* Register a tool that exists on aimee's MCP surface and should also be callable
 * by aimee's own agents, placing it in `toolset` (an existing builtin set).
 *
 * The MCP dispatch table is the single source of truth for which tools exist; the
 * server walks it at startup and registers every entry marked native. This tier
 * cannot link the server tier, so the names arrive by registration rather than by
 * a direct read — the same shape as the git-write and forge provider seams.
 *
 * Both KNOWN_TOOLS (does this name exist?) and the builtin toolsets (may this role
 * call it?) grow from these registrations, so a tool declared once in the MCP table
 * becomes callable everywhere without a second edit. That is the point: the four
 * registries this collapses had to agree by hand, and when they silently disagreed
 * the tool was advertised and uncallable, or absent from aimee's own agents while
 * every external client had it.
 *
 * MUST be called before the first toolset_registry_init(). Unregistered (thin
 * client, unit tests) the list is empty and every toolset resolves exactly as
 * before — no test needs to know this exists. */
void toolset_register_native_tool(const char *name, const char *toolset);

#endif /* DEC_TOOLSET_H */
