/* css_render_cmd.c: command-driven render backend. See css_render_cmd.h. */
#include "css_render_cmd.h"

#include "cJSON.h"
#include "config.h"
#include "css_render_oracle.h"
#include "log.h"
#include "platform_process.h"

#include <stdlib.h>
#include <string.h>

/* Adapter: feed {"html","css"} to css_render_command on stdin, return its stdout
 * (the computed-style snapshot JSON). Returns 0 on success (*out_json set), non-0
 * on failure (*err set). Matches css_render_adapter_fn. */
static int css_render_cmd_adapter(const char *html, const char *css, char **out_json, char **err)
{
   if (out_json)
      *out_json = NULL;
   if (err)
      *err = NULL;

   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.css_render_command[0])
   {
      if (err)
         *err = strdup("css_render_command not configured");
      return 1;
   }

   cJSON *in = cJSON_CreateObject();
   if (!in)
      return 1;
   cJSON_AddStringToObject(in, "html", html ? html : "");
   cJSON_AddStringToObject(in, "css", css ? css : "");
   char *input = cJSON_PrintUnformatted(in);
   cJSON_Delete(in);
   if (!input)
      return 1;

   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe(cfg.css_render_command, input, strlen(input), &out, &out_len);
   free(input);

   if (rc != 0)
   {
      free(out);
      if (err)
      {
         char msg[128];
         snprintf(msg, sizeof(msg), "css_render_command failed (exit %d)", rc);
         *err = strdup(msg);
      }
      return 1;
   }
   if (!out || out_len == 0)
   {
      free(out);
      if (err)
         *err = strdup("css_render_command produced no output");
      return 1;
   }
   if (out_json)
      *out_json = out;
   else
      free(out);
   return 0;
}

int css_render_cmd_register(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 0;
   if (!cfg.css_style_graph_enabled || !cfg.css_render_command[0])
      return 0;
   css_render_oracle_set_adapter(css_render_cmd_adapter);
   LOG_INFO("css_render", "registered command render backend: %s", cfg.css_render_command);
   return 1;
}
