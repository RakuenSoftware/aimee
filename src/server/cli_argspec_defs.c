/* cli_argspec_defs.c: serve the argument specs.
 *
 * The manifest could already tell a thin client which commands exist, how they
 * route, what they are called, and which take no arguments. What it could not
 * say was how the words after the command become fields — so a NEW command that
 * took arguments still needed a new client. This closes that: the marshal rows
 * now carry a spec object as well as the "none" case.
 *
 * The specs live in cli_argspec_defs_data.h, which the differential test
 * includes too, so what is proven is what is served.
 */
#include "cli_argspec_defs.h"

#include "cJSON.h"

#include <stddef.h>

typedef struct
{
   const char *method;
   const char *spec; /* a JSON object, as text */
} cli_argspec_def_t;

static const cli_argspec_def_t g_argspecs[] = {
#include "cli_argspec_defs_data.h"
};

size_t cli_argspec_defs_count(void)
{
   return sizeof(g_argspecs) / sizeof(g_argspecs[0]);
}

cJSON *cli_argspec_defs_to_json(void)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (size_t i = 0; i < cli_argspec_defs_count(); i++)
   {
      const cli_argspec_def_t *d = &g_argspecs[i];
      if (!d->method || !d->spec)
         continue;
      /* Parsed rather than embedded as a string: the client receives an object
       * it can read directly, and a spec that does not parse is dropped HERE
       * rather than shipped for every client to fail on identically. */
      cJSON *spec = cJSON_Parse(d->spec);
      if (!spec)
         continue;
      cJSON *row = cJSON_CreateObject();
      if (!row)
      {
         cJSON_Delete(spec);
         continue;
      }
      cJSON_AddStringToObject(row, "method", d->method);
      cJSON_AddItemToObject(row, "args", spec);
      cJSON_AddItemToArray(arr, row);
   }
   return arr;
}
