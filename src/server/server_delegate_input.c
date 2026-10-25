#include "server_delegate_input.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static char *task_view(const aimee_request_t *request)
{
   const aimee_message_t *message = NULL;
   for (int i = request->n_messages - 1; i >= 0; --i)
      if (!strcmp(request->messages[i].role, "user"))
      {
         message = &request->messages[i];
         break;
      }
   if (!message)
      return NULL;
   size_t size = 1;
   for (int i = 0; i < message->n_blocks; ++i)
   {
      const aimee_block_t *block = &message->blocks[i];
      if (block->type == AIMEE_BLK_TEXT && block->text &&
          block->context.authority != AIMEE_CTX_AUTH_EVIDENCE &&
          aimee_ir_block_model_visible(block))
         size += strlen(block->text) + 1;
   }
   char *text = calloc(size, 1);
   if (!text)
      return NULL;
   size_t used = 0;
   for (int i = 0; i < message->n_blocks; ++i)
   {
      const aimee_block_t *block = &message->blocks[i];
      if (block->type == AIMEE_BLK_TEXT && block->text &&
          block->context.authority != AIMEE_CTX_AUTH_EVIDENCE &&
          aimee_ir_block_model_visible(block))
      {
         if (used)
            text[used++] = '\n';
         size_t length = strlen(block->text);
         memcpy(text + used, block->text, length);
         used += length;
      }
   }
   if (!text[0])
   {
      free(text);
      return strdup("Canonical delegate task");
   }
   return text;
}

int server_delegate_input_parse(const cJSON *envelope, int allow_text, aimee_request_t *out,
                                char **task_text)
{
   const cJSON *canonical = cJSON_GetObjectItemCaseSensitive(envelope, "request");
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(envelope, "message");
   const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(envelope, "prompt");
   const cJSON *system = cJSON_GetObjectItemCaseSensitive(envelope, "system_prompt");
   aimee_request_t source = {0};
   int rc = -1;
   if (canonical)
   {
      if (message || prompt || system || aimee_ir_request_parse(canonical, &source))
         goto done;
   }
   else if (message)
   {
      aimee_message_t input = {0};
      if (prompt || aimee_ir_message_parse(message, &input))
         goto done;
      rc = aimee_ir_request_from_message(&source, cJSON_GetStringValue(system), &input, 0, 0.3);
      aimee_message_free(&input);
      if (rc)
         goto done;
   }
   else if (!allow_text || !cJSON_IsString(prompt) ||
            aimee_ir_request_from_text(&source, cJSON_GetStringValue(system), prompt->valuestring,
                                       0, 0.3))
      goto done;
   char *text = task_view(&source);
   if (!text)
   {
      free(text);
      rc = -1;
      goto done;
   }
   aimee_request_free(out);
   *out = source;
   source = (aimee_request_t){0};
   free(*task_text);
   *task_text = text;
   rc = 0;
done:
   aimee_request_free(&source);
   return rc;
}

int server_delegate_input_has_policy(const aimee_request_t *request)
{
   for (int i = 0; i < request->n_system; ++i)
      if (request->system[i].context.authority == AIMEE_CTX_AUTH_POLICY)
         return 1;
   return 0;
}

size_t server_delegate_input_task_size(const aimee_request_t *request)
{
   for (int i = request->n_messages - 1; i >= 0; --i)
   {
      const aimee_message_t *message = &request->messages[i];
      if (strcmp(message->role, "user"))
         continue;
      size_t length = 0;
      int text_blocks = 0;
      for (int j = 0; j < message->n_blocks; ++j)
      {
         const aimee_block_t *block = &message->blocks[j];
         if (block->type == AIMEE_BLK_TEXT && block->text)
         {
            length += strlen(block->text);
            ++text_blocks;
         }
      }
      /* The legacy minimum is a text-origin guard, not a restriction on rich
       * canonical input. Non-text payloads are admitted by the IR codec. */
      return text_blocks ? length : (message->n_blocks ? 20 : 0);
   }
   return 0;
}


void server_delegate_input_attenuate(aimee_request_t *request)
{
   for (int i = 0; i < request->n_system; ++i)
      aimee_ir_block_attenuate_public(&request->system[i], 1);
   for (int i = 0; i < request->n_messages; ++i)
      for (int j = 0; j < request->messages[i].n_blocks; ++j)
         aimee_ir_block_attenuate_public(&request->messages[i].blocks[j],
                         !strcmp(request->messages[i].role, "system") ||
                                 !strcmp(request->messages[i].role, "developer")
                             ? 2
                             : 0);
   /* Native sidecars cannot resurrect pre-attenuation authority. */
   request->mutated = 1;
}

int server_delegate_request_prepare(const aimee_request_t *source, const char *host_policy,
                                    int max_tokens, aimee_request_t *out)
{
   aimee_request_t request = {0}, policy = {0};
   if (aimee_ir_request_clone(source, &request))
      return -1;
   if (host_policy && host_policy[0])
   {
      if (aimee_ir_request_from_text(&policy, host_policy, "", 0, 0.3) ||
          request.n_system >= AIMEE_IR_MAX_BLOCKS)
         goto fail;
      aimee_block_t *blocks = realloc(request.system, (request.n_system + 1) * sizeof *blocks);
      if (!blocks)
         goto fail;
      request.system = blocks;
      memmove(blocks + 1, blocks, request.n_system * sizeof *blocks);
      blocks[0] = policy.system[0];
      policy.system[0] = (aimee_block_t){0};
      ++request.n_system;
      request.mutated = 1;
   }
   if (max_tokens > 0)
   {
      request.max_tokens = max_tokens;
      request.has_max_tokens = 1;
   }
   if (aimee_ir_request_admit(&request))
      goto fail;
   aimee_request_free(out);
   *out = request;
   aimee_request_free(&policy);
   return 0;
fail:
   aimee_request_free(&policy);
   aimee_request_free(&request);
   return -1;
}
