/* The CLI supplies arguments and renders the Go owner's result. Profile-pack
 * parsing, validation, selection and text formatting live in the memory module. */
#include "aimee.h"
#include "cmd_memory_internal.h"
#include "commands.h"
#include "module_commands.h"
#include "json_fluent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mem_pack(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory pack requires list, show <name>, validate <path> or use <name>");
   cJSON *args = cJSON_CreateObject();
   if (!args || !cJSON_AddStringToObject(args, "action", argv[0]) ||
       (argc > 1 && !cJSON_AddStringToObject(
                        args, strcmp(argv[0], "validate") == 0 ? "path" : "name", argv[1])))
   {
      cJSON_Delete(args);
      fatal("out of memory");
   }
   cJSON *result = NULL;
   int rc = aimee_module_commands_dispatch("memory.pack", args, &result);
   cJSON_Delete(args);
   if (rc != 1 || !result)
   {
      cJSON_Delete(result);
      fatal("memory module unavailable");
   }
   int failed = strcmp(jo_cstr(result, "status"), "ok") != 0;
   if (ctx->json_output)
   {
      cJSON *output = failed ? result : cJSON_DetachItemFromObjectCaseSensitive(result, "output");
      if (!failed)
         cJSON_Delete(result);
      emit_json_ctx(output, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (failed)
         fprintf(stderr, "error: %s\n", jo_cstr(result, "message"));
      else
         fputs(jo_cstr(result, "text"), stdout);
      cJSON_Delete(result);
   }
   if (failed)
      exit(1);
}
