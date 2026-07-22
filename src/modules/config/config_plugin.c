/* config_plugin.c: plugin and context engine config extensions.
 *
 * Parses keys that were excluded from config.c due to line-count constraints.
 * Called from server_main.c after config_load().
 */
#include "config.h"
#include "headers/aimee_home.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse context.engine and other plugin extension keys from the config file
 * into an already-loaded config_t.  Non-fatal: silently skips on parse error. */
void config_load_plugin_extensions(config_t *cfg)
{
   if (!cfg)
      return;

   const char *home = aimee_home();
   if (!home || !home[0])
      return;

   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);

   FILE *fp = fopen(path, "r");
   if (!fp)
      return;

   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return;
   }
   size_t nr = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[nr] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return;

   /* context.engine: name */
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(root, "context");
   if (cJSON_IsObject(ctx))
   {
      cJSON *eng = cJSON_GetObjectItemCaseSensitive(ctx, "engine");
      if (cJSON_IsString(eng) && eng->valuestring && eng->valuestring[0])
         snprintf(cfg->context_engine, sizeof(cfg->context_engine), "%s", eng->valuestring);
   }

   cJSON_Delete(root);
}
