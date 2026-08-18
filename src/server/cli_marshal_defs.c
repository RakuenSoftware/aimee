/* cli_marshal_defs.c: the methods whose request body is empty.
 *
 * The thin client refuses a command it cannot marshal, so serving routes, the
 * catalogue and the dispatch rows still left a NEW no-argument command
 * unusable: the client had every piece except the one saying "this takes no
 * arguments", and answered "arguments are missing or invalid, so no request was
 * sent". That row is pure data, so it can be served like the rest.
 *
 * Deliberately only the no-arg set. Of the 42 marshaller functions, 34 map argv
 * to fields and need a spec carrying types before they can follow; the other 8
 * read the client's own disk or environment (marshal_git_cli,
 * marshal_skill_request, marshal_index_file_request, ...) and should not follow
 * at all -- reading local state to send it is the thin client's own job, not
 * knowledge of what the server can do.
 */
#include "cli_marshal_defs.h"

#include "cJSON.h"

#include <stddef.h>

static const char *const g_no_arg_methods[] = {
#include "cli_marshal_defs_data.h"
};

size_t cli_marshal_defs_count(void)
{
   return sizeof(g_no_arg_methods) / sizeof(g_no_arg_methods[0]);
}

cJSON *cli_marshal_defs_to_json(void)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (size_t i = 0; i < cli_marshal_defs_count(); i++)
   {
      if (!g_no_arg_methods[i])
         continue;
      cJSON *row = cJSON_CreateObject();
      if (!row)
         continue;
      cJSON_AddStringToObject(row, "method", g_no_arg_methods[i]);
      /* "none" rather than a bare boolean, so this field has room to describe
       * real argument shapes later without a wire break. */
      cJSON_AddStringToObject(row, "args", "none");
      cJSON_AddItemToArray(arr, row);
   }
   return arr;
}
