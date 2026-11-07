/* delegate_depth.c: CLI delegation chain depth guard.
 *
 * Tracks delegation nesting depth via the AIMEE_DELEGATE_DEPTH environment
 * variable so that child processes spawned by a delegate agent inherit the
 * current depth and the limit can be enforced across process boundaries.
 */
#include "platform_process.h"
#include "cmd_agent_delegate_impl.h"
#include <aimee/delegates/module_api.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static delegate_chain_provider_fn g_chain_provider;

void delegate_register_chain_provider(delegate_chain_provider_fn provider)
{
   g_chain_provider = provider;
}
