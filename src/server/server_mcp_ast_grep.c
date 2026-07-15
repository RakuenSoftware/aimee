/* server_mcp_ast_grep.c: split from server_mcp.c into a real translation unit
 * (was server_mcp_ast_grep.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "server_mcp_internal.h"
#include "server.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "dstr.h"
#include "commands.h"
#include "db2/curiosity.h"
#include "memory.h"
#include "index.h"
#include "code_span.h"
#include "db1.h"
#include "kb_client.h"
#include "dashboard.h"
#include "mcp_tools.h"
#include "mcp_git.h"
#include "git_verify.h"
#include "workspace_turn.h"
#include "notes.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "agent_pipeline.h"
#include "delegate_economics.h"
#include "delegate_patch_coordinator.h"
#include "platform_path.h"
#include "lsp.h"
#include "server_mcp_learning.h"
#include "server_mcp_process.h"
#include "server_mcp_skill.h"
#include "server_mcp_delegate.h"
#include "server_mcp_ensemble.h"
#include "wfe_advance_exec.h"  /* advance_request interactive-driver executor (S2) */
#include "wfe_block_resolve.h" /* per-block externalization guard (S2 sub-slice 4) */
#include "server_mcp_gateway.h"
#include "server_http.h"
#include "server_pipeline.h" /* handle_pipeline_* for the pipeline.* MCP tools */
#include "headers/conversation_context.h"
#include "headers/payload_rewrite.h"
#include "headers/session_search_tool.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>
#include "agent_help_data.h"

/* --- ast_grep_search --- */

/* Resolve the sg (ast-grep) binary path.
 * Checks ~/.local/bin/sg first (where install.sh places it), then falls back
 * to "sg" in PATH via execvp. Returns a pointer to a static buffer. */
static const char *ast_grep_binary(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   const char *home = platform_home_dir();
   if (home && home[0])
   {
      snprintf(path, sizeof(path), "%s/.local/bin/sg", home);
      if (access(path, X_OK) == 0)
         return path;
   }

   /* Fall back to name-only; execvp will search PATH */
   snprintf(path, sizeof(path), "sg");
   return path;
}

#define AST_GREP_MAX_OUTPUT (256 * 1024)

cJSON *tool_ast_grep_search(cJSON *args)
{
   cJSON *jpat = cJSON_GetObjectItemCaseSensitive(args, "pattern");
   cJSON *jlang = cJSON_GetObjectItemCaseSensitive(args, "lang");
   cJSON *jpath = cJSON_GetObjectItemCaseSensitive(args, "path");

   if (!cJSON_IsString(jpat) || !jpat->valuestring[0])
      return text_content("error: missing 'pattern' parameter");
   if (!cJSON_IsString(jlang) || !jlang->valuestring[0])
      return text_content("error: missing 'lang' parameter");

   const char *pattern = jpat->valuestring;
   const char *lang = jlang->valuestring;
   const char *path = (cJSON_IsString(jpath) && jpath->valuestring[0]) ? jpath->valuestring : ".";

   const char *sg = ast_grep_binary();
   const char *argv[] = {sg, "--json", "--pattern", pattern, "--lang", lang, path, NULL};

   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, AST_GREP_MAX_OUTPUT);

   /* exit code 127 from execvp means binary not found */
   if (rc == 127 || (!output && rc != 0))
   {
      free(output);
      return text_content("error: ast-grep binary (sg) not found. "
                          "Install it with: curl -fsSL "
                          "https://github.com/ast-grep/ast-grep/releases/latest/download/"
                          "sg-x86_64-unknown-linux-musl.tar.gz | tar xz -C ~/.local/bin");
   }

   if (!output || !output[0])
   {
      free(output);
      return text_content("No matches found.");
   }

   /* Parse NDJSON output: each line is a JSON object with file, range, text */
   char result[AST_GREP_MAX_OUTPUT];
   int rpos = 0;
   int match_count = 0;

   char *line = output;
   while (*line)
   {
      char *end = strchr(line, '\n');
      if (end)
         *end = '\0';

      if (*line == '{')
      {
         cJSON *m = cJSON_Parse(line);
         if (m)
         {
            cJSON *jfile = cJSON_GetObjectItem(m, "file");
            cJSON *jrange = cJSON_GetObjectItem(m, "range");
            cJSON *jtext = cJSON_GetObjectItem(m, "text");
            cJSON *jlines = cJSON_GetObjectItem(m, "lines");

            const char *file = cJSON_IsString(jfile) ? jfile->valuestring : "?";
            const char *display = (cJSON_IsString(jlines) && jlines->valuestring[0])
                                      ? jlines->valuestring
                                  : cJSON_IsString(jtext) ? jtext->valuestring
                                                          : "";
            int line_no = 0;
            if (jrange)
            {
               cJSON *jstart = cJSON_GetObjectItem(jrange, "start");
               if (jstart)
               {
                  cJSON *jln = cJSON_GetObjectItem(jstart, "line");
                  if (cJSON_IsNumber(jln))
                     line_no = (int)jln->valuedouble + 1; /* ast-grep is 0-indexed */
               }
            }

            /* Trim trailing newlines from display text */
            char display_buf[512];
            snprintf(display_buf, sizeof(display_buf), "%s", display);
            size_t dlen = strlen(display_buf);
            while (dlen > 0 && (display_buf[dlen - 1] == '\n' || display_buf[dlen - 1] == '\r'))
               display_buf[--dlen] = '\0';

            rpos += snprintf(result + rpos, sizeof(result) - (size_t)rpos, "%s:%d: %s\n", file,
                             line_no, display_buf);
            match_count++;
            cJSON_Delete(m);
         }
      }

      if (!end)
         break;
      line = end + 1;
   }

   free(output);

   if (match_count == 0)
      return text_content("No matches found.");

   char header[64];
   snprintf(header, sizeof(header), "Found %d match(es):\n\n", match_count);
   size_t hlen = strlen(header);
   size_t rlen = (size_t)rpos;

   char *combined = malloc(hlen + rlen + 1);
   if (!combined)
      return text_content(result);

   memcpy(combined, header, hlen);
   memcpy(combined + hlen, result, rlen);
   combined[hlen + rlen] = '\0';

   cJSON *content = text_content(combined);
   free(combined);
   return content;
}
