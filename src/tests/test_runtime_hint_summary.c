/* Exercise the actual pure summary builder without starting the economizer bus. */
#include <assert.h>
#include "agent_runtime_messages.h"
#include "../server/session_compact.c"

cJSON *econ_module_record_build(const cJSON *messages, int start, int end, char **closet,
                                int *evict)
{
   (void)messages;
   (void)start;
   (void)end;
   (void)closet;
   (void)evict;
   abort(); /* from_record=0 must never cross the unrelated module boundary. */
}

int main(void)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON_AddItemToArray(messages, agent_session_text_message("user", "genuine user task"));
   cJSON_AddItemToArray(messages, agent_session_text_message("assistant", "genuine model answer"));
   cJSON_AddItemToArray(messages, agent_session_control_message("platform progress advisory"));
   cJSON_AddItemToArray(messages,
                        agent_session_tool_evidence_message("untrusted repository advisory"));
   compact_history_t history = {0};
   assert(!history_load(messages, &history));
   char summary[SESSION_COMPACT_SUMMARY_MAX];
   assert(!build_summary(messages, &history, 0, history.count, summary, sizeof summary, 0));
   assert(strstr(summary, "Observed 1 assistant turn in the summarised range."));
   assert(strstr(summary, "Latest assistant note: genuine model answer"));
   assert(!strstr(summary, "Latest assistant note: platform progress advisory"));
   assert(!strstr(summary, "Latest assistant note: untrusted repository advisory"));
   history_free(&history);
   cJSON_Delete(messages);
   puts("runtime hint summary: passed");
   return 0;
}
