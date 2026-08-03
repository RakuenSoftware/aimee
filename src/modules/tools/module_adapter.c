#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/tools/module_api.h>

#include <string.h>

static int one_of(const char *name, const char *const *values)
{
   for (size_t i = 0; values[i]; ++i)
      if (strcmp(name, values[i]) == 0)
         return 1;
   return 0;
}

static aimee_tool_class_t classify(const char *name)
{
   static const char *const exec[] = {"bash", "execute_script", "test", "run_tests", NULL};
   static const char *const write[] = {"write_file", "edit_file", "edit_symbol", "create_note",
                                       "rules_propose", "learning_propose", "git_commit",
                                       "git_push", "git_branch", "git_pr", NULL};
   static const char *const control[] = {"request_input", "clarify_start", "clarify_answer",
                                         "diagnose_start", "diagnose_observe",
                                         "diagnose_hypothesize", "diagnose_evidence", NULL};
   static const char *const read[] = {"read_file", "list_files", "grep", "code_search",
                                      "find_symbol", "read_symbol", "search_memory",
                                      "search_docs", "web_search", "web_read", "list_notes",
                                      "search_notes", "git_log", "git_diff", "git_status", NULL};
   if (strchr(name, ':'))
      return AIMEE_TOOL_CLASS_REMOTE;
   if (one_of(name, exec))
      return AIMEE_TOOL_CLASS_EXEC;
   if (one_of(name, write))
      return AIMEE_TOOL_CLASS_WRITE;
   if (one_of(name, control))
      return AIMEE_TOOL_CLASS_CONTROL;
   if (one_of(name, read))
      return AIMEE_TOOL_CLASS_READ;
   return AIMEE_TOOL_CLASS_UNKNOWN;
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   char name[AIMEE_TOOLS_NAME_MAX + 1];
   if (!invocation || !response_len || invocation->stage_id != AIMEE_TOOLS_STAGE_DISPATCH ||
       response_capacity < AIMEE_TOOLS_RESPONSE_LEN ||
       aimee_tools_request_decode(request_body, request_len, name, sizeof(name)) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   aimee_tools_put_u32(response_body, AIMEE_TOOLS_RESPONSE_MAGIC);
   aimee_tools_put_u32(response_body + 4, (uint32_t)classify(name));
   *response_len = AIMEE_TOOLS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
