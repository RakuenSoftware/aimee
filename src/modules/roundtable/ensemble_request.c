#include "ensemble_request.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ensemble_synthesis_request(const aimee_request_t *task, const char *instructions,
                               aimee_request_t *out)
{
   if (!instructions || aimee_ir_request_admit(task) || aimee_ir_request_clone(task, out))
      return -1;
   if (out->n_messages >= AIMEE_IR_MAX_BLOCKS)
      goto fail;
   aimee_message_t *messages =
       realloc(out->messages, (size_t)(out->n_messages + 1) * sizeof *messages);
   if (!messages)
      goto fail;
   out->messages = messages;
   aimee_message_t *message = &messages[out->n_messages++];
   *message = (aimee_message_t){.role = strdup("user")};
   if (!message->role)
      goto fail;
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
   /* Generated labels/instructions may enclose domain retrieval material.
    * Keep that wrapper unverified; participant model blocks are appended intact. */
   aimee_ir_block_set_context(added, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_EVIDENCE,
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

/* Canonical block ownership crosses the model boundary intact. Terminal usage,
 * diagnostics and native response-root metadata stay with the source result. */
int ensemble_request_add_response(aimee_request_t *request, const char *label,
                                  const aimee_response_t *response)
{
   if (!request || aimee_ir_response_complete_text_valid(response))
      return -1;
   aimee_request_t next = {0};
   if (ensemble_synthesis_request(request, label, &next))
      return -1;
   aimee_response_t copy = {0};
   if (aimee_ir_response_clone(response, &copy) || next.n_messages >= AIMEE_IR_MAX_BLOCKS)
      goto fail;
   if (!copy.role && !(copy.role = strdup("assistant")))
      goto fail;
   aimee_message_t *messages =
       realloc(next.messages, (size_t)(next.n_messages + 1) * sizeof *messages);
   if (!messages)
      goto fail;
   next.messages = messages;
   messages[next.n_messages++] =
       (aimee_message_t){.role = copy.role, .blocks = copy.content, .n_blocks = copy.n_content};
   copy.role = NULL;
   copy.content = NULL;
   copy.n_content = 0;
   if (aimee_ir_request_admit(&next))
      goto fail;
   aimee_response_free(&copy);
   aimee_request_free(request);
   *request = next;
   return 0;
fail:
   aimee_response_free(&copy);
   aimee_request_free(&next);
   return -1;
}
