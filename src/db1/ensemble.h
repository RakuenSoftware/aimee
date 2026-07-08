/* db1/ensemble.h: templated multi-agent ensembles.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_ENSEMBLE_H
#define DEC_DB1_ENSEMBLE_H 1

#include "aimee.h"
#include "cJSON.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define ENSEMBLE_TEMPLATE_NAME_MAX 64
#define ENSEMBLE_CHANNEL_NAME_MAX  64
#define ENSEMBLE_AGENT_NAME_MAX    64
#define ENSEMBLE_ROLE_NAME_MAX     64
#define ENSEMBLE_PHASE_NAME_MAX    64

   typedef struct
   {
      int id;
      char template_name[ENSEMBLE_TEMPLATE_NAME_MAX];
      char channel[ENSEMBLE_CHANNEL_NAME_MAX];
      char status[16];
      int current_phase;
      int current_turn;
      int phase_count;
      int turns_in_phase;
      char phase_name[ENSEMBLE_PHASE_NAME_MAX];
      char expected_agent[ENSEMBLE_AGENT_NAME_MAX];
      char expected_role[ENSEMBLE_ROLE_NAME_MAX];
      char paused_reason[64];
      char created_at[32];
      char updated_at[32];
   } ensemble_info_t;

   /* Template helpers (no DB). */
   int db1_ensemble_template_path(const char *project_root, const char *name, char *buf,
                                  size_t bufsz);
   cJSON *db1_ensemble_template_load(const char *project_root, const char *name, char *err,
                                     size_t errlen);
   int db1_ensemble_role_needs_dissent(const char *role);

   int db1_ensemble_create(const char *project_root, const char *template_name, const char *channel,
                           cJSON *assignments, int *out_id, char *err, size_t errlen);
   int db1_ensemble_get(int id, ensemble_info_t *out, char **prompt_out, char **context_out,
                        char *err, size_t errlen);
   int db1_ensemble_pause(int id, const char *reason, char *err, size_t errlen);
   int db1_ensemble_advance(int id, const char *sender, const char *text, ensemble_info_t *out,
                            char **prompt_out, char *err, size_t errlen);
   int db1_ensemble_list(ensemble_info_t **out, int *out_count, char *err, size_t errlen);
   int db1_ensemble_find_current_by_channel(const char *channel, int *out_id, char *err,
                                            size_t errlen);

   /* Build a JSON object describing a ensemble suitable for MCP/HTTP responses.
    * prompt_text / context_text may be NULL. Caller owns returned cJSON*. */
   cJSON *db1_ensemble_info_to_json(const ensemble_info_t *info, const char *prompt_text,
                                    const char *context_text);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_ENSEMBLE_H */
