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
      const char *active_turn_id; /* Server-owned capture turn; never client input. */
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
   /* Browser opening reconciles abandoned owned native custody before replay and
    * may acknowledge a same-session legacy source. Never starts a model/tool turn. */
   int primary_session_history_open(const char *session, const char *actor);
   void primary_session_adapter_reset(void);
   /* Host startup installs its authenticated session recovery owner. Standalone
    * adapters can only admit a complete empty native owner list. No weak fallback. */
   typedef int (*primary_session_native_reconcile_fn)(const char *session, const char *actor,
                                                      const char *active_turn, char error[512]);
   void primary_session_adapter_set_native_reconcile(primary_session_native_reconcile_fn fn);
   /* Browse recovery must admit healthy live owners without disrupting their
    * control/replay stream; abandoned custody still requires exact host proof. */
   typedef int (*primary_session_native_browse_reconcile_fn)(const char *session, const char *actor,
                                                             char error[512]);
   void primary_session_adapter_set_native_browse_reconcile(
       primary_session_native_browse_reconcile_fn fn);
   /* Only the authenticated host loader may return histories containing protected
    * interruption diagnostics. Both output arrays transfer to the adapter. */
   typedef int (*primary_session_native_history_load_fn)(const char *session, const char *actor,
                                                         const char *active_turn,
                                                         int include_running,
                                                         struct cJSON **messages,
                                                         struct cJSON **receipts);
   void primary_session_adapter_set_native_history_load(primary_session_native_history_load_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PRIMARY_SESSION_ADAPTER_H */
