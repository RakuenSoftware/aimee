/* ensemble_view.c: the caller's side of the two ensemble reads.
 *
 * db1_ensemble_view and _advance_view return the row, the turn prompt and the
 * ensemble's verdict in a single reply, because they are a single observation
 * and asking for them separately would let a turn land in between. These two
 * functions unpack that reply into the out-parameters callers already use, so
 * the shape of the boundary changed without the callers changing with it.
 *
 * The verdict arrives as data rather than as a failed call: the store worked,
 * and what it has to report is that the ensemble said no. Only a returned -1
 * from the view itself means the store could not answer.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ensemble.h"

static int unpack(ensemble_view_t *view, int rc, ensemble_info_t *out, char **prompt_out,
                  char **context_out, char *err, size_t errlen)
{
   if (rc != 0)
   {
      /* The store could not answer at all, so there is no verdict to relay. */
      if (err && errlen)
         snprintf(err, errlen, "ensemble store unavailable");
      free(view->prompt);
      free(view->context);
      return -1;
   }
   if (err && errlen)
      snprintf(err, errlen, "%s", view->err);
   if (view->rc != 0)
   {
      free(view->prompt);
      free(view->context);
      return view->rc;
   }
   if (out)
      *out = view->info;
   if (prompt_out)
      *prompt_out = view->prompt;
   else
      free(view->prompt);
   if (context_out)
      *context_out = view->context;
   else
      free(view->context);
   return 0;
}

int db1_ensemble_get(int id, ensemble_info_t *out, char **prompt_out, char **context_out, char *err,
                     size_t errlen)
{
   ensemble_view_t view;
   memset(&view, 0, sizeof view);
   int rc = db1_ensemble_view(id, &view);
   return unpack(&view, rc, out, prompt_out, context_out, err, errlen);
}

int db1_ensemble_advance(int id, const char *sender, const char *text, ensemble_info_t *out,
                         char **prompt_out, char *err, size_t errlen)
{
   ensemble_view_t view;
   memset(&view, 0, sizeof view);
   int rc = db1_ensemble_advance_view(id, sender, text, &view);
   return unpack(&view, rc, out, prompt_out, NULL, err, errlen);
}

/* The same unpacking for the two operations that answer with an id. */
static int unpack_id(const ensemble_id_result_t *res, int rc, int *out_id, char *err, size_t errlen)
{
   if (rc != 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "ensemble store unavailable");
      return -1;
   }
   if (err && errlen)
      snprintf(err, errlen, "%s", res->err);
   if (res->rc != 0)
      return res->rc;
   if (out_id)
      *out_id = res->id;
   return 0;
}

int db1_ensemble_create(const char *project_root, const char *config_dir, const char *template_name,
                        const char *channel, const char *assignments_json, int *out_id, char *err,
                        size_t errlen)
{
   ensemble_id_result_t res;
   memset(&res, 0, sizeof res);
   int rc = db1_ensemble_create_id(project_root, config_dir, template_name, channel,
                                   assignments_json, &res);
   return unpack_id(&res, rc, out_id, err, errlen);
}

int db1_ensemble_find_current_by_channel(const char *channel, int *out_id, char *err, size_t errlen)
{
   ensemble_id_result_t res;
   memset(&res, 0, sizeof res);
   int rc = db1_ensemble_find_current_id(channel, &res);
   return unpack_id(&res, rc, out_id, err, errlen);
}

int db1_ensemble_list(ensemble_info_t **out, int *out_count, char *err, size_t errlen)
{
   if (db1_ensemble_list_rows(out, out_count) != 0)
   {
      /* The store's own message does not cross: every failure here is the
         store failing rather than an answer about any one ensemble. */
      if (err && errlen)
         snprintf(err, errlen, "failed to list ensembles");
      return -1;
   }
   return 0;
}
