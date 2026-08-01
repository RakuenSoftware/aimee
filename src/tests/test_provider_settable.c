/* test_provider_settable.c: unit tests for provider_name_settable().
 *
 * Regression: handle_provider_set persisted ANY string under 16 chars as the
 * durable primary provider. The CLI route table maps `provider` with a NULL
 * subcommand to provider.set, so an unrecognized `aimee provider <word>` — e.g.
 * `aimee provider capability` — silently rewrote the primary, and every later
 * chat turn then failed with "provider 'capability' is not configured as an
 * aimee agent". */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "agent_types.h"
#include "server.h"

static void add_agent(agent_config_t *acfg, const char *name, const char *provider)
{
   agent_t *a = &acfg->agents[acfg->agent_count++];
   memset(a, 0, sizeof(*a));
   snprintf(a->name, sizeof(a->name), "%s", name);
   snprintf(a->provider, sizeof(a->provider), "%s", provider);
}

/* The reported failure: a non-provider token must be rejected, not persisted. */
static void test_rejects_unknown_token(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   add_agent(&acfg, "MiniMax-M3", "anthropic");

   assert(provider_name_settable("capability", &acfg) == 0);
   assert(provider_name_settable("models", &acfg) == 0);
   assert(provider_name_settable("quota", &acfg) == 0);
   printf("  PASS: unknown token is not settable as a provider\n");
}

/* A configured agent is settable by its NAME or by its PROVIDER — the same two
 * ways chat_agent_select_provider resolves a primary. */
static void test_accepts_configured_agent(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   add_agent(&acfg, "MiniMax-M3", "anthropic");
   add_agent(&acfg, "kimi-k2.7-code", "anthropic");

   assert(provider_name_settable("MiniMax-M3", &acfg) == 1); /* by name */
   assert(provider_name_settable("anthropic", &acfg) == 1);  /* by provider */
   assert(provider_name_settable("kimi-k2.7-code", &acfg) == 1);
   printf("  PASS: a configured agent is settable by name or provider\n");
}

/* Built-in CLI providers stay settable even with no agent configured for them —
 * server_compute synthesizes those on demand. */
static void test_accepts_builtin_providers(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));

   assert(provider_name_settable("claude", &acfg) == 1);
   assert(provider_name_settable("claude-oauth", &acfg) == 1);
   assert(provider_name_settable("codex-oauth", &acfg) == 1);
   assert(provider_name_settable("openai", &acfg) == 1);
   printf("  PASS: built-in CLI providers are settable with no agent configured\n");
}

/* A known adapter is settable even when absent from agents.json. */
static void test_accepts_known_adapter(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   assert(provider_name_settable("minimax", &acfg) == 1);
   printf("  PASS: a known adapter is settable\n");
}

static void test_rejects_empty_and_null(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   assert(provider_name_settable(NULL, &acfg) == 0);
   assert(provider_name_settable("", &acfg) == 0);
   /* A NULL config still permits built-ins, but not an agent-only name. */
   assert(provider_name_settable("claude", NULL) == 1);
   assert(provider_name_settable("MiniMax-M3", NULL) == 0);
   printf("  PASS: empty/NULL names rejected; NULL config allows only built-ins\n");
}

int main(void)
{
   printf("test_provider_settable:\n");
   test_rejects_unknown_token();
   test_accepts_configured_agent();
   test_accepts_builtin_providers();
   test_accepts_known_adapter();
   test_rejects_empty_and_null();
   printf("test_provider_settable: all tests passed\n");
   return 0;
}
