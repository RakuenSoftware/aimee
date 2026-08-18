/* cli_dispatch_defs.c: the CLI dispatch rows the thin client resolves against.
 *
 * `aimee <group> <verb>` -> the method the server routes. These were compiled
 * into the client, which is why a command added server-side stayed uninvokable
 * from an existing client even after its route and its help were served: the
 * client had no row mapping the typed words to a method.
 *
 * Served here so that stops being true. The client keeps a copy compiled from
 * the same data file as a last resort (dispatch has to work with no server
 * reachable, the same way help does), and a served row always wins.
 */
#include "cli_dispatch_defs.h"

#include "cJSON.h"

#include <stddef.h>

typedef struct
{
   const char *cmd;
   /* NULL = match any first arg (the command takes no subcommand keyword);
    * "" = matches only when there is no subcommand. The two are NOT the same,
    * and the wire form below has to keep them apart. */
   const char *subcmd;
   const char *method;
   const char *server_method; /* NULL = same as method */
   const char *extract;       /* response array field to extract, or NULL */
   int timeout_ms;            /* 0 = client default */
} cli_dispatch_def_t;

static const cli_dispatch_def_t g_cli_dispatch[] = {
#include "cli_dispatch_defs_data.h"
};

size_t cli_dispatch_defs_count(void)
{
   return sizeof(g_cli_dispatch) / sizeof(g_cli_dispatch[0]);
}

cJSON *cli_dispatch_defs_to_json(void)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (size_t i = 0; i < cli_dispatch_defs_count(); i++)
   {
      const cli_dispatch_def_t *d = &g_cli_dispatch[i];
      if (!d->cmd) /* sentinel */
         continue;
      cJSON *row = cJSON_CreateObject();
      if (!row)
         continue;
      cJSON_AddStringToObject(row, "cmd", d->cmd);
      /* Absent key = the NULL wildcard; present-and-empty = "no subcommand".
       * Collapsing these would silently change which commands match. */
      if (d->subcmd)
         cJSON_AddStringToObject(row, "sub", d->subcmd);
      cJSON_AddStringToObject(row, "method", d->method ? d->method : "");
      if (d->server_method)
         cJSON_AddStringToObject(row, "server_method", d->server_method);
      if (d->extract)
         cJSON_AddStringToObject(row, "extract", d->extract);
      /* Omitted means "client default", which is what 0 means in the table. */
      if (d->timeout_ms > 0)
         cJSON_AddNumberToObject(row, "timeout_ms", d->timeout_ms);
      cJSON_AddItemToArray(arr, row);
   }
   return arr;
}
