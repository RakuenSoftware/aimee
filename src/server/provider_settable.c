/* server/provider_settable.c: which provider names may be persisted as the
 * durable primary. Pure policy (no I/O), so it is unit tested directly. */
#include <string.h>

#include "aimee.h"
#include "agent_adapter.h"
#include "agent_types.h"
#include "server.h"

/* The built-in CLI providers server_compute synthesizes on demand when no agent
 * is configured for them (chat_agent_add_builtin_provider). Keep in sync. */
static int provider_is_builtin_name(const char *name)
{
   static const char *builtins[] = {"openai",       "claude", "claude-code",
                                    "claude-oauth", "codex",  "codex-oauth"};
   for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
      if (strcmp(name, builtins[i]) == 0)
         return 1;
   return 0;
}

/* A provider is settable only if the chat path can actually RESOLVE it: a
 * configured agent (matched by name or by provider, as chat_agent_select_provider
 * does), a known adapter, or a built-in CLI provider.
 *
 * Without this check any string under 16 chars was persisted as the durable
 * primary, and every later chat turn died with "provider '...' is not configured
 * as an aimee agent" — with no hint of what had set it. The CLI makes that easy
 * to trigger by accident: the route table maps `provider` with a NULL subcommand
 * to provider.set (cli_v1_routes.c), so any unrecognized `aimee provider <word>`
 * — e.g. a mistyped `aimee provider capability` — silently rewrote the primary
 * instead of reporting an unknown subcommand. Validating here fixes it for every
 * client (CLI, webchat, scripts), not just the one that happened to send it. */
int provider_name_settable(const char *name, const agent_config_t *acfg)
{
   if (!name || !name[0])
      return 0;
   if (provider_is_builtin_name(name))
      return 1;
   if (agent_adapter_for_provider(name))
      return 1;
   if (!acfg)
      return 0;
   for (int i = 0; i < acfg->agent_count; i++)
      if (strcmp(acfg->agents[i].name, name) == 0 || strcmp(acfg->agents[i].provider, name) == 0)
         return 1;
   return 0;
}

