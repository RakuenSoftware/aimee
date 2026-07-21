/* extension.h: Required extension ABI and typed runtime registries. */
#ifndef AIMEE_MODULE_RUNTIME_EXTENSION_H
#define AIMEE_MODULE_RUNTIME_EXTENSION_H 1

#define PLUGIN_MAX_HOOKS       16
#define PLUGIN_MAX_TOOLS       16
#define PLUGIN_MAX_PERMISSIONS 8
#define PLUGIN_MAX_PLUGINS     64

struct delegate_backend;
typedef struct delegate_backend delegate_backend_t;

struct memory_provider;
typedef struct memory_provider memory_provider_t;

struct context_engine;
typedef struct context_engine context_engine_t;

typedef enum
{
   PLUGIN_KIND_STANDALONE = 0,
   PLUGIN_KIND_BACKEND,
   PLUGIN_KIND_EXCLUSIVE,
   PLUGIN_KIND_PLATFORM
} plugin_kind_t;

const char *plugin_kind_name(plugin_kind_t kind);
plugin_kind_t plugin_kind_from_str(const char *value);

typedef enum
{
   PLUGIN_PERM_READ = 0,
   PLUGIN_PERM_WRITE,
   PLUGIN_PERM_EXECUTE,
   PLUGIN_PERM_DANGEROUS
} plugin_permission_t;

const char *plugin_permission_name(plugin_permission_t permission);
plugin_permission_t plugin_permission_from_str(const char *value);

typedef struct
{
   char event[32];    /* PreToolUse or PostToolUse */
   char command[512]; /* handler path */
} plugin_hook_t;

typedef struct
{
   char name[64];
   char description[512];
   char command[512];
   char input_schema_json[2048];
   plugin_permission_t permission;
} plugin_tool_t;

typedef struct
{
   char name[64];
   char description[256];
   char command[512];
} plugin_slash_cmd_t;

typedef struct
{
   char name[64];
   char description[256];
   char command[512];
} plugin_cli_cmd_t;

typedef struct
{
   char name[64];
   char api_url[256];
   char model_name[128];
   char api_key_env[64];
   int default_provider;
} plugin_model_provider_t;

typedef struct
{
   char name[64];
   char description[256];
   char webhook_endpoint[512];
   int enabled;
} plugin_platform_adapter_t;

typedef struct
{
   char name[64];
   char description[256];
   char connection_string[512];
   int is_default;
} plugin_memory_provider_t;

typedef struct
{
   char name[64];
   char description[256];
   int is_default;
} plugin_context_engine_t;

typedef struct plugin_ctx plugin_ctx_t;
struct plugin_ctx
{
   /* Set by the loader before it invokes the extension entry point. */
   const char *plugin_name;
   const char *plugin_version;
   const char *source_path;
   plugin_kind_t kind;

   int (*register_tool)(plugin_ctx_t *ctx, const plugin_tool_t *tool);
   int (*register_hook)(plugin_ctx_t *ctx, const plugin_hook_t *hook);
   int (*register_slash_command)(plugin_ctx_t *ctx, const plugin_slash_cmd_t *cmd);
   int (*register_cli_subcommand)(plugin_ctx_t *ctx, const plugin_cli_cmd_t *cmd);
   int (*register_delegate_backend)(plugin_ctx_t *ctx, delegate_backend_t *backend);
   int (*register_platform_adapter)(plugin_ctx_t *ctx, const plugin_platform_adapter_t *adapter);
   int (*register_memory_provider)(plugin_ctx_t *ctx, const memory_provider_t *provider);
   int (*register_context_engine)(plugin_ctx_t *ctx, const context_engine_t *engine);
   int (*register_model_provider)(plugin_ctx_t *ctx, const plugin_model_provider_t *provider);

   int (*on_init)(plugin_ctx_t *ctx);      /* startup after registration */
   void (*on_shutdown)(plugin_ctx_t *ctx); /* cleanup before destruction */
};

plugin_ctx_t *plugin_ctx_create(void);
void plugin_ctx_destroy(plugin_ctx_t *ctx);

/* Concurrent mutation is unsupported. Register during single-threaded startup. */
extern plugin_tool_t plugin_tools[PLUGIN_MAX_TOOLS];
extern int plugin_tool_count;
extern plugin_hook_t plugin_hooks[PLUGIN_MAX_HOOKS];
extern int plugin_hook_count;
extern delegate_backend_t *plugin_delegate_backends[PLUGIN_MAX_PLUGINS];
extern int plugin_delegate_backend_count;
extern plugin_platform_adapter_t plugin_platform_adapters[PLUGIN_MAX_PLUGINS];
extern int plugin_platform_adapter_count;
extern plugin_memory_provider_t plugin_memory_providers[PLUGIN_MAX_PLUGINS];
extern int plugin_memory_provider_count;
extern plugin_context_engine_t plugin_context_engines[PLUGIN_MAX_PLUGINS];
extern int plugin_context_engine_count;
extern plugin_cli_cmd_t plugin_cli_subcommands[PLUGIN_MAX_PLUGINS];
extern int plugin_cli_subcommand_count;
extern plugin_slash_cmd_t plugin_slash_commands[PLUGIN_MAX_PLUGINS];
extern int plugin_slash_command_count;
extern plugin_model_provider_t plugin_model_providers[PLUGIN_MAX_PLUGINS];
extern int plugin_model_provider_count;

#endif /* AIMEE_MODULE_RUNTIME_EXTENSION_H */
