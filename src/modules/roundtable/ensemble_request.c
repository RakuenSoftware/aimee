#include "ensemble_request.h"
#include <stdlib.h>
#include <string.h>

int ensemble_synthesis_request(const aimee_request_t *task, const char *instructions,
                               aimee_request_t *out)
{
   if (!task)
      return aimee_ir_request_from_text(out, NULL, instructions, 0, 0.3);
   if (!instructions || aimee_ir_request_admit(task) || task->n_messages != 1 ||
       strcmp(task->messages[0].role, "user") || aimee_ir_request_clone(task, out))
      return -1;
   aimee_message_t *message = &out->messages[0];
   aimee_block_t *blocks =
       realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks);
   if (!blocks)
      goto fail;
   message->blocks = blocks;
   aimee_block_t *added = &blocks[message->n_blocks++];
   *added = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(instructions)};
   if (!added->text)
      goto fail;
   aimee_ir_block_set_context(added, AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   out->mutated = 1;
   if (aimee_ir_request_admit(out))
      goto fail;
   return 0;
fail:
   aimee_request_free(out);
   return -1;
}
