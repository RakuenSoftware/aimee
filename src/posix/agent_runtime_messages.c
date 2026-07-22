#include "agent_runtime_messages.h"
#include <stdio.h>

void agent_session_append_final_message(cJSON *messages, const char *content)
{
   if (!messages || !content || !content[0])
      return;

   cJSON *asst = cJSON_CreateObject();
   if (!asst)
      return;
   cJSON_AddStringToObject(asst, "role", "assistant");
   cJSON_AddStringToObject(asst, "content", content);
   cJSON_AddItemToArray(messages, asst);
}

void agent_session_append_final_instruction(cJSON *messages)
{
   if (!messages)
      return;

   cJSON *msg = cJSON_CreateObject();
   if (!msg)
      return;
   cJSON_AddStringToObject(msg, "role", "user");
   cJSON_AddStringToObject(
       msg, "content",
       "[FINAL RESPONSE REQUIRED] The tool turn budget is exhausted. Do not call tools, do not "
       "emit XML tool_call blocks, and do not request more inspection. Return the best final "
       "answer now from the evidence already gathered. Clearly note any uncertainty or missing "
       "verification.");
   cJSON_AddItemToArray(messages, msg);
}

void agent_session_append_final_retry_instruction(cJSON *messages, const char *attempted_action)
{
   if (!messages)
      return;

   cJSON *msg = cJSON_CreateObject();
   if (!msg)
      return;
   cJSON_AddStringToObject(msg, "role", "user");
   char content[1024];
   snprintf(content, sizeof(content),
            "[FINAL RESPONSE RETRY] Your previous response returned or attempted %s after the "
            "tool budget was closed. Aimee is retrying automatically. Do not call tools, do "
            "not emit XML tool_call blocks, and do not request more inspection. Return the "
            "best final answer now from the evidence already gathered. Clearly note any "
            "uncertainty or missing verification.",
            attempted_action && attempted_action[0] ? attempted_action : "a tool action");
   cJSON_AddStringToObject(msg, "content", content);
   cJSON_AddItemToArray(messages, msg);
}

void agent_session_append_degenerate_retry_instruction(cJSON *messages)
{
   if (!messages)
      return;

   cJSON *msg = cJSON_CreateObject();
   if (!msg)
      return;
   cJSON_AddStringToObject(msg, "role", "user");
   cJSON_AddStringToObject(
       msg, "content",
       "[DELEGATE RESPONSE RETRY] Your previous response was raw tool-call markup or "
       "degenerate text before any tool executed. Aimee is retrying automatically. If "
       "inspection is needed, use the available tool interface so Aimee can execute it. "
       "Do not emit XML, bracket, or dollar-prefixed tool-call markup as plain text. If "
       "no tool is needed, return a plain prose answer.");
   cJSON_AddItemToArray(messages, msg);
}

int agent_session_retry_final_tool_violation(cJSON *messages, const char *attempted_action,
                                             int *turn, int *max_t, int initial_max_t,
                                             int *retry_count, char *error, size_t error_len)
{
   if (!messages || !turn || !max_t || !retry_count)
      return 0;
   if (*retry_count >= AGENT_FINAL_TOOL_RETRY_LIMIT)
   {
      if (error && error_len > 0)
         snprintf(error, error_len,
                  "model repeatedly attempted tool calls on the forced final response turn");
      return 0;
   }

   (*retry_count)++;
   agent_session_append_final_retry_instruction(messages, attempted_action);

   if (*turn >= *max_t - 1 && *max_t < initial_max_t + AGENT_FINAL_TOOL_RETRY_LIMIT)
      (*max_t)++;
   (*turn)++;
   return 1;
}

int agent_session_retry_degenerate_response(cJSON *messages, int *turn, int *retry_count,
                                            int *force_text_only_retry)
{
   if (!messages || !turn || !retry_count || !force_text_only_retry)
      return 0;
   if (*retry_count >= AGENT_DEGENERATE_RESPONSE_RETRY_LIMIT)
      return 0;
   (*retry_count)++;
   *force_text_only_retry = 1;
   agent_session_append_degenerate_retry_instruction(messages);
   (*turn)++;
   return 1;
}
