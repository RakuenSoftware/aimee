#ifndef DEC_PRIMARY_SESSION_ADAPTER_H
#define DEC_PRIMARY_SESSION_ADAPTER_H 1

#include "aimee.h"
#include "agent_types.h"
#include "session_compact.h"

struct cJSON;

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct primary_session_request
   {
      const agent_t *agent;
      const agent_network_t *network;
      const char *provider_session_id;
      const char *aimee_session_id;
      const char *cwd;
      const char *system_prompt;
      const char *user_prompt;
      /* Borrowed native user request, for bounded Go-owned task obligations. */
      const struct cJSON *task_request;
      int max_tokens;
      double temperature;
   } primary_session_request_t;

   int primary_session_adapter_can_handle(const agent_t *agent);
   int primary_session_adapter_turn(const primary_session_request_t *req, agent_result_t *out,
                                    char *session_id_out, size_t session_id_len);
   int primary_session_adapter_compact(const primary_session_request_t *req,
                                       session_compact_result_t *result, char *session_id_out,
                                       size_t session_id_len, char *error_out, size_t error_len);
   /* Compact by session ID alone — for use from MCP tools where no agent struct
    * is available. Finds the session by ID, compacts if it has history, and
    * persists the result. Returns 0 on success, -1 on error. */
   int primary_session_adapter_compact_by_session_id(const char *session_id,
                                                     session_compact_result_t *result_out,
                                                     char *error_out, size_t error_len);
   void primary_session_adapter_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PRIMARY_SESSION_ADAPTER_H */
