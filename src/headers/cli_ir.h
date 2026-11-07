#ifndef AIMEE_CLI_IR_H
#define AIMEE_CLI_IR_H
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <string.h>

/* User text becomes IR here, before first-party transport. File/context bytes
 * remain separately typed; no downstream reader must split a composed prompt. */
static inline int cli_ir_user_blocks(aimee_block_t blocks[3], const char *task,
                                     const char *file_task, const char *evidence)
{
   const char *texts[] = {task, file_task, evidence};
   int count = 0;
   for (int i = 0; i < 3; ++i)
      if (texts[i] && texts[i][0])
      {
         blocks[count] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = (char *)texts[i]};
         aimee_ir_block_set_context(
             &blocks[count], AIMEE_CTX_ORIGIN_USER,
             i == 2 ? AIMEE_CTX_AUTH_EVIDENCE : AIMEE_CTX_AUTH_TASK_INSTRUCTION,
             AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
         count++;
      }
   return count;
}
static inline cJSON *cli_ir_user_message(const char *task, const char *file_task,
                                         const char *evidence)
{
   aimee_block_t blocks[3] = {0};
   int count = cli_ir_user_blocks(blocks, task, file_task, evidence);
   if (!count)
      return NULL;
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = count};
   return aimee_ir_message_json(&message);
}

static inline cJSON *cli_ir_request(const char *task, const char *file_task, const char *evidence,
                                    const char *policy, int max_tokens)
{
   aimee_block_t blocks[3] = {0};
   int count = cli_ir_user_blocks(blocks, task, file_task, evidence);
   if (!count)
      return NULL;
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = count};
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = (char *)policy};
   aimee_ir_block_set_context(&system, AIMEE_CTX_ORIGIN_OPERATOR, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_request_t request = {.messages = &message,
                              .n_messages = 1,
                              .system = policy && policy[0] ? &system : NULL,
                              .n_system = policy && policy[0] ? 1 : 0,
                              .max_tokens = max_tokens > 0 ? max_tokens : 0,
                              .has_max_tokens = max_tokens > 0};
   return aimee_ir_request_json(&request);
}

static inline int cli_ir_add_request(cJSON *envelope, const char *task, const char *file_task,
                                     const char *evidence, const char *policy, int max_tokens)
{
   cJSON *request = cli_ir_request(task, file_task, evidence, policy, max_tokens);
   if (!request || !cJSON_AddItemToObject(envelope, "request", request))
   {
      cJSON_Delete(request);
      return -1;
   }
   return 0;
}
#endif
