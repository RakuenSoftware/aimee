/* plugin_stubs.c: no-op plugin API for builds with AIMEE_WITH_PLUGIN_LOADER=0.
 *
 * The plugin manifest loader is an optional, default-OFF module (nothing ships a
 * plugin; extensions are expected to be first-class modules). Its real sources
 * (the modules/plugin-loader and module-runtime extension sources) compile only under
 * =1. Server/CLI/MCP/dashboard code still references the plugin API unconditionally,
 * so this translation unit provides inert definitions when the loader is omitted:
 * discovery finds nothing, the registry is empty, and management ops fail closed.
 * Compiled ONLY in the =0 profile (see the Makefile CORE_SRCS gate) so it never
 * collides with the real definitions. */
#include <aimee/plugin-loader/plugin.h>
#include <aimee/module-runtime/extension.h>
#include <stddef.h>

int plugin_discover_local(const char **workspace_roots, int root_count, plugin_t *plugins,
                          int *count, int max)
{
   (void)workspace_roots; (void)root_count; (void)plugins; (void)max;
   if (count) *count = 0;
   return 0;
}
int plugin_registry_load(plugin_t *plugins, int max) { (void)plugins; (void)max; return 0; }
int plugin_collect_tools(const plugin_t *plugins, int count, plugin_tool_t *out, int max_tools)
{ (void)plugins; (void)count; (void)out; (void)max_tools; return 0; }
int plugin_collect_hooks(const plugin_t *plugins, int count, const char *event, char cmds[][512],
                         int max)
{ (void)plugins; (void)count; (void)event; (void)cmds; (void)max; return 0; }
int plugin_manifest_parse(const char *dir, plugin_t *out, char *err_buf, size_t err_len)
{ (void)dir; (void)out; if (err_buf && err_len) err_buf[0] = '\0'; return -1; }
int plugin_install(const char *source_dir, char *err_buf, size_t err_len)
{ (void)source_dir; if (err_buf && err_len) err_buf[0] = '\0'; return -1; }
int plugin_remove(const char *name, char *err_buf, size_t err_len)
{ (void)name; if (err_buf && err_len) err_buf[0] = '\0'; return -1; }
int plugin_set_enabled(const char *name, int enabled, char *err_buf, size_t err_len)
{ (void)name; (void)enabled; if (err_buf && err_len) err_buf[0] = '\0'; return -1; }
char *plugin_registry_json(void) { return NULL; }
const char *plugin_kind_name(plugin_kind_t kind) { (void)kind; return "standalone"; }
const char *plugin_permission_name(plugin_permission_t permission) { (void)permission; return ""; }
plugin_kind_t plugin_kind_from_str(const char *value) { (void)value; return PLUGIN_KIND_STANDALONE; }
plugin_permission_t plugin_permission_from_str(const char *value) { (void)value; return (plugin_permission_t)0; }
plugin_ctx_t *plugin_ctx_create(void) { return NULL; }
void plugin_ctx_destroy(plugin_ctx_t *ctx) { (void)ctx; }
