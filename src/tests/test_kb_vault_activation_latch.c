#include "kb/kb_vault_activation_latch.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static kb_vault_operator_status_t operational(void)
{
   kb_vault_operator_status_t status;
   memset(&status, 0, sizeof(status));
   status.state = KB_VAULT_OPERATOR_STATE_OPERATIONAL;
   status.operation_state = KB_VAULT_OPERATOR_OPERATION_NONE;
   status.remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE;
   status.seal_epoch = 7;
   status.control_fence = 9;
   return status;
}

int main(void)
{
   kb_vault_activation_latch_t latch;
   kb_vault_operator_status_t out;
   assert(kb_vault_activation_latch_init(&latch) == 0);
   assert(kb_vault_activation_latch_wait(&latch, 1, &out) == 0);
   kb_vault_operator_status_t status = operational();
   assert(kb_vault_activation_latch_publish(&latch, &status) == 0);
   memset(&out, 0, sizeof(out));
   assert(kb_vault_activation_latch_wait(&latch, 1, &out) == 1);
   assert(memcmp(&out, &status, sizeof(out)) == 0);
   assert(kb_vault_activation_latch_publish(&latch, &status) == 0);
   status.control_fence++;
   assert(kb_vault_activation_latch_publish(&latch, &status) != 0);
   kb_vault_activation_latch_destroy(&latch);
   puts("kb vault activation latch tests passed");
   return 0;
}
