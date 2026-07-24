/* plugin_internal.h: plugin-loader contract shared across the module's own
 * translation units (plugin.c, plugin_loader.c) and the focused plugin tests.
 *
 * These symbols have no caller outside the module, so they are kept out of the
 * public include/aimee/plugin-loader/ headers. plugin.c and plugin_loader.c
 * include this as a sibling ("plugin_internal.h"); tests reach it through the
 * source root ("modules/plugin-loader/plugin_internal.h").
 */
#ifndef AIMEE_PLUGIN_LOADER_PLUGIN_INTERNAL_H
#define AIMEE_PLUGIN_LOADER_PLUGIN_INTERNAL_H

#include "aimee/plugin-loader/plugin.h"

/* Find a plugin by name in the registry. Returns 0 on success, -1 if absent. */
int plugin_registry_get(const char *name, plugin_t *out);

/* Load a plugin from its source_path, create a plugin_ctx_t, and call the
 * plugin's register(ctx) entry point. The plugin's on_init runs after
 * registration succeeds. Returns 0 on success, -1 on error (sets err_buf). */
int plugin_load_and_register(const plugin_t *plugin, char *err_buf, size_t err_len);

/* Check if a tool name conflicts with a built-in. Returns 1 if conflict, 0 otherwise. */
int plugin_tool_conflicts_with_builtin(const char *tool_name);

#endif /* AIMEE_PLUGIN_LOADER_PLUGIN_INTERNAL_H */
