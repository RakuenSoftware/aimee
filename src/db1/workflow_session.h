/* db1/workflow_session.h: templated multi-agent workflow sessions.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_WORKFLOW_SESSION_H
#define DEC_DB1_WORKFLOW_SESSION_H 1

#include "aimee.h"
#include "cJSON.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define WF_TEMPLATE_NAME_MAX 64
#define WF_CHANNEL_NAME_MAX  64
#define WF_AGENT_NAME_MAX    64
#define WF_ROLE_NAME_MAX     64
#define WF_PHASE_NAME_MAX    64

   typedef struct
   {
      int id;
      char template_name[WF_TEMPLATE_NAME_MAX];
      char channel[WF_CHANNEL_NAME_MAX];
      char status[16];
      int current_phase;
      int current_turn;
      int phase_count;
      int turns_in_phase;
      char phase_name[WF_PHASE_NAME_MAX];
      char expected_agent[WF_AGENT_NAME_MAX];
      char expected_role[WF_ROLE_NAME_MAX];
      char paused_reason[64];
      char created_at[32];
      char updated_at[32];
   } workflow_session_info_t;

   /* Template helpers (no DB). */
   int db1_workflow_template_path(const char *project_root, const char *name, char *buf,
                                  size_t bufsz);
   cJSON *db1_workflow_template_load(const char *project_root, const char *name, char *err,
                                     size_t errlen);
   int db1_workflow_role_needs_dissent(const char *role);

   int db1_workflow_session_create(const char *project_root, const char *template_name,
                                   const char *channel, cJSON *assignments, int *out_id, char *err,
                                   size_t errlen);
   int db1_workflow_session_get(int id, workflow_session_info_t *out, char **prompt_out,
                                char **context_out, char *err, size_t errlen);
   int db1_workflow_session_pause(int id, const char *reason, char *err, size_t errlen);
   int db1_workflow_session_advance(int id, const char *sender, const char *text,
                                    workflow_session_info_t *out, char **prompt_out, char *err,
                                    size_t errlen);
   int db1_workflow_session_list(workflow_session_info_t **out, int *out_count, char *err,
                                 size_t errlen);
   int db1_workflow_session_find_current_by_channel(const char *channel, int *out_id, char *err,
                                                    size_t errlen);

   /* Build a JSON object describing a workflow session suitable for MCP/HTTP responses.
    * prompt_text / context_text may be NULL. Caller owns returned cJSON*. */
   cJSON *db1_workflow_session_info_to_json(const workflow_session_info_t *info,
                                            const char *prompt_text, const char *context_text);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WORKFLOW_SESSION_H */
