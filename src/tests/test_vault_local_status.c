#include "vault_internal.h"
#include "vault_server_key.h"

#include <assert.h>
#include <stdio.h>

typedef struct
{
   int result;
   unsigned observed_timeout;
} status_context_t;

static int local_status(void *opaque, unsigned timeout_ms)
{
   status_context_t *context = opaque;
   context->observed_timeout = timeout_ms;
   return context->result;
}

int main(void)
{
   status_context_t context = {0};
   vault_custody_provider_t provider = {
       .name = "status-test",
       .ctx = &context,
       .local_status = local_status,
   };

   for (int status = VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED; status <= VAULT_CUSTODY_LOCAL_MALFORMED;
        status++)
   {
      context.result = status;
      context.observed_timeout = 0;
      vault_custody_set_provider(&provider);
      assert(vault_custody_selected_local_status() == (vault_custody_local_status_t)status);
      assert(context.observed_timeout == 50);
   }
   context.result = 99;
   assert(vault_custody_selected_local_status() == VAULT_CUSTODY_LOCAL_MALFORMED);
   provider.local_status = NULL;
   vault_custody_set_provider(&provider);
   assert(vault_custody_selected_local_status() == VAULT_CUSTODY_LOCAL_UNAVAILABLE);
   vault_custody_set_provider(NULL);
   assert(vault_custody_selected_local_status() == VAULT_CUSTODY_LOCAL_UNAVAILABLE);
   puts("vault local status tests passed");
   return 0;
}
