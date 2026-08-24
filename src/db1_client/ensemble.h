/* db1/ensemble.h: templated multi-agent ensembles — the persistent, turn-based
 * SESSION mode of the ensemble concept (see docs/ENSEMBLE.md). The one-shot
 * aggregate (Mixture-of-Agents) and roundtable panel modes live in
 * server/delegate_ensemble.{c,h}; a delegate can advance a session bound to a
 * channel via db1_ensemble_find_current_by_channel below.
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
#define ENSEMBLE_ERR_LEN           256

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

   /* What a read of one ensemble returns: the row, the turn prompt built from
    * it, and the verdict.
    *
    * These travel together in one reply because they are one observation. Asked
    * separately -- the row from one call, the prompt from the next -- a turn
    * taken in between would pair turn N's row with turn N+1's prompt, and the
    * result reads as a consistent ensemble that never existed.
    *
    * rc and err carry the ensemble's own answer ("expected 'alice', got 'bob'",
    * "already complete"). Those are verdicts, not store failures, so they ride
    * a successful reply; a broken store is still a failed one. */
   typedef struct
   {
      int rc;
      char err[ENSEMBLE_ERR_LEN];
      ensemble_info_t info;
      char *prompt;
      char *context;
   } ensemble_view_t;

   /* An id and the verdict that produced it. Two operations answer this shape:
    * starting a run and finding the current one for a channel. Both can fail
    * for a reason worth reading ("template 'x' not found", "no ensemble for
    * channel 'y'"), and a bare status would flatten every one of them to -1. */
   typedef struct
   {
      int rc;
      char err[ENSEMBLE_ERR_LEN];
      int id;
   } ensemble_id_result_t;

   int db1_ensemble_create_id(const char *project_root, const char *config_dir,
                              const char *template_name, const char *channel,
                              const char *assignments_json, ensemble_id_result_t *out);
   int db1_ensemble_find_current_id(const char *channel, ensemble_id_result_t *out);

   /* Read one ensemble, and advance one. The public functions below are thin
    * unpackings of these, kept so callers need not change shape. */
   int db1_ensemble_view(int id, ensemble_view_t *out);
   int db1_ensemble_advance_view(int id, const char *sender, const char *text,
                                 ensemble_view_t *out);

   /* Template resolution, phase walking and prompt building are NOT declared
    * here: they have no caller outside ensemble.c and they are the ensemble's
    * behaviour rather than its surface. Publishing them made the boundary look
    * wider than it is.
    *
    * create() takes both roots rather than resolving the second itself. The
    * store is a separate process and cannot read the daemon's configuration --
    * the same reason db1_module_init.c refuses to guess the database path. A
    * module that guessed "the default location" would keep working until an
    * operator moved it, and then quietly resolve templates somewhere else. */
   /* assignments_json rather than a cJSON tree: a pointer does not cross a
    * process boundary, and the caller already holds the document. */
   int db1_ensemble_create(const char *project_root, const char *config_dir,
                           const char *template_name, const char *channel,
                           const char *assignments_json, int *out_id, char *err, size_t errlen);
   int db1_ensemble_get(int id, ensemble_info_t *out, char **prompt_out, char **context_out,
                        char *err, size_t errlen);
   int db1_ensemble_pause(int id, const char *reason, char *err, size_t errlen);
   int db1_ensemble_advance(int id, const char *sender, const char *text, ensemble_info_t *out,
                            char **prompt_out, char *err, size_t errlen);
   /* Unlike the reads above, every way this fails is the store failing --
    * out of memory, or SQLite refusing the query. There is no verdict to
    * relay, so it answers with rows or with nothing. */
   int db1_ensemble_list_rows(ensemble_info_t **out, int *out_count);
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
