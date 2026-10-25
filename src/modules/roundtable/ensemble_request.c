#include "ensemble_request.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ensemble_synthesis_request(const aimee_request_t *task, const char *instructions,
                               aimee_request_t *out)
{
   if (!instructions || aimee_ir_request_admit(task) || aimee_ir_request_clone(task, out))
      return -1;
   aimee_message_t *message = out->n_messages ? &out->messages[out->n_messages - 1] : NULL;
   if (!message || strcmp(message->role, "user") || message->n_blocks == AIMEE_IR_MAX_BLOCKS)
   {
      aimee_message_t *messages =
          realloc(out->messages, (size_t)(out->n_messages + 1) * sizeof *messages);
      if (!messages)
         goto fail;
      out->messages = messages;
      message = &messages[out->n_messages++];
      *message = (aimee_message_t){.role = strdup("user")};
      if (!message->role)
         goto fail;
   }
   aimee_block_t *blocks =
       realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks);
   if (!blocks)
      goto fail;
   message->blocks = blocks;
   message->content_scalar = 0;
   aimee_block_t *added = &blocks[message->n_blocks++];
   *added = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = malloc(strlen(instructions) + 3)};
   if (!added->text)
      goto fail;
   snprintf(added->text, strlen(instructions) + 3, "\n\n%s", instructions);
   /* Context may contain peer output or stored artifacts, so the whole block
    * remains unverified evidence. Only separately generated personas are policy. */
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

int ensemble_continuation_request(const aimee_request_t *task, const char *persona,
                                  const char *context, int max_tokens, double temperature,
                                  aimee_request_t *out)
{
   if (!task || ensemble_synthesis_request(task, context, out))
      return -1;
   if (persona && persona[0])
   {
      aimee_block_t *blocks = realloc(out->system, (size_t)(out->n_system + 1) * sizeof *blocks);
      if (!blocks)
         goto fail;
      out->system = blocks;
      aimee_block_t *added = &blocks[out->n_system++];
      *added = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(persona)};
      if (!added->text)
         goto fail;
      aimee_ir_block_set_context(added, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                                 AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                                 0);
   }
   if (!out->has_max_tokens && max_tokens > 0)
   {
      out->max_tokens = max_tokens;
      out->has_max_tokens = 1;
   }
   if (!out->has_temperature)
   {
      out->temperature = temperature;
      out->has_temperature = 1;
   }
   if (!aimee_ir_request_admit(out))
      return 0;
fail:
   aimee_request_free(out);
   return -1;
}
