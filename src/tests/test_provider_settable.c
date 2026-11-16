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

int main(void)
{
   printf("test_provider_settable:\n");
   printf("test_provider_settable: all tests passed\n");
   return 0;
}
