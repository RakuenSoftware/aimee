/* plugin.h: Optional plugin manifests, persistence, and dynamic loading. */
#ifndef AIMEE_PLUGIN_LOADER_PLUGIN_H
#define AIMEE_PLUGIN_LOADER_PLUGIN_H 1

#include "config.h"
#include "aimee/module-runtime/extension.h"

#define PLUGIN_MANIFEST_FILE ".aimee-plugin/plugin.json"
#define PLUGIN_REGISTRY_FILE "plugins/installed.json"

/* --- Capability flags for plugin.yaml capabilities[] --- */
typedef enum
{
   PLUGIN_CAP_NONE = 0,
   PLUGIN_CAP_TOOLS = 1 << 0,
   PLUGIN_CAP_HOOKS = 1 << 1,
   PLUGIN_CAP_SLASH_COMMANDS = 1 << 2,
   PLUGIN_CAP_CLI_SUBCOMMANDS = 1 << 3,
   PLUGIN_CAP_DELEGATE_BACKEND = 1 << 4,
   PLUGIN_CAP_MEMORY_PROVIDER = 1 << 5,
   PLUGIN_CAP_CONTEXT_ENGINE = 1 << 6,
   PLUGIN_CAP_MODEL_PROVIDER = 1 << 7,
   PLUGIN_CAP_PLATFORM_ADAPTER = 1 << 8
} plugin_capability_t;

/* --- Full plugin descriptor (in-memory representation of plugin.json) --- */
typedef struct
{
   char name[64];
   char version[32];
   char description[256];
   char source_path[MAX_PATH_LEN]; /* absolute path to plugin directory */
   int enabled;
   plugin_kind_t kind;       /* taxonomy: standalone/backend/exclusive/platform */
   plugin_capability_t caps; /* ORed PLUGIN_CAP_* flags */

   plugin_hook_t hooks[PLUGIN_MAX_HOOKS];
   int hook_count;

   plugin_tool_t tools[PLUGIN_MAX_TOOLS];
   int tool_count;

   int default_enabled;

   /* Extended manifest fields */
   char capabilities_json[2048];                  /* raw JSON array from manifest */
   char config_schema_json[4096];                 /* JSON schema for plugin config */
   char required_env[PLUGIN_MAX_PERMISSIONS][64]; /* required env var names */
   int required_env_count;
} plugin_t;

/* --- Plugin registry API --- */

/* Load all plugins from the registry into plugins[].
 * Returns count (0 means empty registry; not an error). */
int plugin_registry_load(plugin_t *plugins, int max);

/* Return a malloc'd JSON array describing installed plugins. Caller frees. */
char *plugin_registry_json(void);

/* --- Manifest parsing --- */

/* Parse plugin.json at dir/PLUGIN_MANIFEST_FILE into out.
 * source_path is set to dir.
 * Also supports plugin.yaml (YAML format) if available.
 * Returns 0 on success, -1 on error (sets err_buf). */
int plugin_manifest_parse(const char *dir, plugin_t *out, char *err_buf, size_t err_len);

/* --- Install / enable / disable / remove --- */

/* Install a plugin from source_dir.
 * Reads the manifest, checks for name conflicts, appends to registry.
 * Returns 0 on success, -1 on error. */
int plugin_install(const char *source_dir, char *err_buf, size_t err_len);

/* Enable or disable a plugin by name (0 = disable, 1 = enable). */
int plugin_set_enabled(const char *name, int enabled, char *err_buf, size_t err_len);

/* Remove a plugin by name from the registry. */
int plugin_remove(const char *name, char *err_buf, size_t err_len);

/* --- Project-local plugins --- */

/* Scan workspace_roots for .aimee-plugin/ directories and add them to
 * plugins[] if not already registered. Returns count of newly added. */
int plugin_discover_local(const char **workspace_roots, int root_count, plugin_t *plugins,
                          int *count, int max);

/* --- Hook aggregation --- */

/* Collect all PreToolUse or PostToolUse hook commands from enabled plugins
 * into cmds[]. Returns count of collected commands.
 * event should be "PreToolUse" or "PostToolUse". */
int plugin_collect_hooks(const plugin_t *plugins, int count, const char *event, char cmds[][512],
                         int max);

/* --- Tool access --- */

/* Collect all tools from enabled plugins into out[]. Returns count. */
int plugin_collect_tools(const plugin_t *plugins, int count, plugin_tool_t *out, int max_tools);

#endif /* AIMEE_PLUGIN_LOADER_PLUGIN_H */
