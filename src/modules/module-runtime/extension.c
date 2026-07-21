/* extension.c: Required extension ABI and typed runtime registries. */
#include "aimee/module-runtime/extension.h"
#include <stdlib.h>
#include <string.h>

const char *plugin_kind_name(plugin_kind_t kind)
{
   switch (kind)
   {
   case PLUGIN_KIND_STANDALONE:
      return "standalone";
   case PLUGIN_KIND_BACKEND:
      return "backend";
   case PLUGIN_KIND_EXCLUSIVE:
      return "exclusive";
   case PLUGIN_KIND_PLATFORM:
      return "platform";
   default:
      return "standalone";
   }
}

plugin_kind_t plugin_kind_from_str(const char *value)
{
   if (!value)
      return PLUGIN_KIND_STANDALONE;
   if (strcmp(value, "backend") == 0)
      return PLUGIN_KIND_BACKEND;
   if (strcmp(value, "exclusive") == 0)
      return PLUGIN_KIND_EXCLUSIVE;
   if (strcmp(value, "platform") == 0)
      return PLUGIN_KIND_PLATFORM;
   return PLUGIN_KIND_STANDALONE;
}

const char *plugin_permission_name(plugin_permission_t permission)
{
   switch (permission)
   {
   case PLUGIN_PERM_READ:
      return "read";
   case PLUGIN_PERM_WRITE:
      return "write";
   case PLUGIN_PERM_EXECUTE:
      return "execute";
   case PLUGIN_PERM_DANGEROUS:
      return "dangerous";
   default:
      return "read";
   }
}

plugin_permission_t plugin_permission_from_str(const char *value)
{
   if (!value)
      return PLUGIN_PERM_READ;
   if (strcmp(value, "write") == 0)
      return PLUGIN_PERM_WRITE;
   if (strcmp(value, "execute") == 0)
      return PLUGIN_PERM_EXECUTE;
   if (strcmp(value, "dangerous") == 0)
      return PLUGIN_PERM_DANGEROUS;
   return PLUGIN_PERM_READ;
}

plugin_tool_t plugin_tools[PLUGIN_MAX_TOOLS];
int plugin_tool_count;
plugin_hook_t plugin_hooks[PLUGIN_MAX_HOOKS];
int plugin_hook_count;
delegate_backend_t *plugin_delegate_backends[PLUGIN_MAX_PLUGINS];
int plugin_delegate_backend_count;
plugin_platform_adapter_t plugin_platform_adapters[PLUGIN_MAX_PLUGINS];
int plugin_platform_adapter_count;
plugin_memory_provider_t plugin_memory_providers[PLUGIN_MAX_PLUGINS];
int plugin_memory_provider_count;
plugin_context_engine_t plugin_context_engines[PLUGIN_MAX_PLUGINS];
int plugin_context_engine_count;
plugin_cli_cmd_t plugin_cli_subcommands[PLUGIN_MAX_PLUGINS];
int plugin_cli_subcommand_count;
plugin_slash_cmd_t plugin_slash_commands[PLUGIN_MAX_PLUGINS];
int plugin_slash_command_count;
plugin_model_provider_t plugin_model_providers[PLUGIN_MAX_PLUGINS];
int plugin_model_provider_count;

static int register_tool(plugin_ctx_t *ctx, const plugin_tool_t *tool)
{
   (void)ctx;
   if (!tool || plugin_tool_count >= PLUGIN_MAX_TOOLS)
      return -1;
   plugin_tools[plugin_tool_count++] = *tool;
   return 0;
}

static int register_hook(plugin_ctx_t *ctx, const plugin_hook_t *hook)
{
   (void)ctx;
   if (!hook || plugin_hook_count >= PLUGIN_MAX_HOOKS)
      return -1;
   plugin_hooks[plugin_hook_count++] = *hook;
   return 0;
}

static int register_slash_command(plugin_ctx_t *ctx, const plugin_slash_cmd_t *cmd)
{
   (void)ctx;
   if (!cmd || plugin_slash_command_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_slash_commands[plugin_slash_command_count++] = *cmd;
   return 0;
}

static int register_cli_subcommand(plugin_ctx_t *ctx, const plugin_cli_cmd_t *cmd)
{
   (void)ctx;
   if (!cmd || plugin_cli_subcommand_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_cli_subcommands[plugin_cli_subcommand_count++] = *cmd;
   return 0;
}

static int register_delegate_backend(plugin_ctx_t *ctx, delegate_backend_t *backend)
{
   (void)ctx;
   if (!backend || plugin_delegate_backend_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_delegate_backends[plugin_delegate_backend_count++] = backend;
   return 0;
}

static int register_platform_adapter(plugin_ctx_t *ctx, const plugin_platform_adapter_t *adapter)
{
   (void)ctx;
   if (!adapter || plugin_platform_adapter_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_platform_adapters[plugin_platform_adapter_count++] = *adapter;
   return 0;
}

static int register_model_provider(plugin_ctx_t *ctx, const plugin_model_provider_t *provider)
{
   (void)ctx;
   if (!provider || plugin_model_provider_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_model_providers[plugin_model_provider_count++] = *provider;
   return 0;
}

plugin_ctx_t *plugin_ctx_create(void)
{
   plugin_ctx_t *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

   ctx->register_tool = register_tool;
   ctx->register_hook = register_hook;
   ctx->register_slash_command = register_slash_command;
   ctx->register_cli_subcommand = register_cli_subcommand;
   ctx->register_delegate_backend = register_delegate_backend;
   ctx->register_platform_adapter = register_platform_adapter;
   ctx->register_model_provider = register_model_provider;
   /* The optional loader installs memory/context bridges before register(). */
   return ctx;
}

void plugin_ctx_destroy(plugin_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->on_shutdown)
      ctx->on_shutdown(ctx);
   free(ctx);
}
