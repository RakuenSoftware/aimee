/* Minimal stub of provider_cli_adapter_get for test targets that link
 * delegate_routing.o — it calls this for the tmux-CLI context-window fallback in
 * agent_meets_filter — but do not exercise that path and must not pull in the
 * full CLI adapter machinery (cli_codex.o et al.). Returning NULL makes the
 * fallback a no-op, preserving each test's pre-fallback behavior. The real
 * lookup is covered by unit-test-provider-cli-adapter and unit-test-cmd-delegate
 * (the latter stubs it with a codex window to exercise the fallback). */
#include "provider_cli_adapter.h"

const provider_cli_adapter_t *provider_cli_adapter_get(const char *cli_kind)
{
   (void)cli_kind;
   return NULL;
}
