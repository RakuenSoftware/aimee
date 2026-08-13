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
#include "aimee/protocols/mcp/mcp_tools.h"
#include "modules/git/mcp_git.h"
#include "modules/git/git_verify.h"
#include "modules/workspace/workspace_turn.h"
#include "notes.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "agent_pipeline.h"
#include <aimee/delegates/delegate_economics.h>
#include <aimee/delegates/delegate_patch_coordinator.h>
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

/* Is `candidate` really ast-grep? `--version` prints "ast-grep <semver>". */
static int is_ast_grep(const char *candidate)
{
   const char *argv[] = {candidate, "--version", NULL};
   char *out = NULL;
   int rc = safe_exec_capture(argv, &out, 4096);
   int ok = (rc == 0) && out && strstr(out, "ast-grep") != NULL;
   free(out);
   return ok;
}

/* Resolve the ast-grep binary, VERIFYING it is ast-grep before returning it.
 * NULL when no ast-grep is installed, so the caller can say so.
 *
 * `sg` IS ALSO A STANDARD LINUX COMMAND -- util-linux's set-group-ID runner --
 * and it is present on essentially every distro. The old resolver checked
 * ~/.local/bin/sg and otherwise fell back to bare "sg", so on any box without
 * ast-grep installed, execvp found /usr/bin/sg instead. What happened next was
 * worse than an error:
 *
 *   util-linux sg rejects --json and writes to stderr; safe_exec_capture merges
 *   stderr into the captured output, so `output` is NON-empty; the not-found
 *   guard is (rc == 127 || (!output && rc != 0)) and neither half fires; the
 *   error text is then parsed as NDJSON, matches nothing, and the tool answers
 *   "No matches found."
 *
 * A wrong binary reported as a clean empty result. An agent asking "does this
 * pattern repeat anywhere" was told no, authoritatively, on a machine where the
 * search never ran. That is the worst possible failure for a search tool, and it
 * is invisible in a transcript.
 *
 * So: prefer the unambiguous name (ast-grep), accept sg only where it verifies,
 * and verify with --version rather than trusting the filename. Cached after the
 * first resolution -- the probe costs one subprocess per process lifetime. */
static const char *ast_grep_binary(void)
{
   static char path[MAX_PATH_LEN];
   static int resolved = 0;
   if (resolved)
      return path[0] ? path : NULL;
   resolved = 1;

   const char *home = platform_home_dir();
   char home_ast[MAX_PATH_LEN] = "";
   char home_sg[MAX_PATH_LEN] = "";
   if (home && home[0])
   {
      snprintf(home_ast, sizeof(home_ast), "%s/.local/bin/ast-grep", home);
      snprintf(home_sg, sizeof(home_sg), "%s/.local/bin/sg", home);
   }

   /* Explicit paths first (an install we placed), then PATH names. "ast-grep"
    * before "sg" because only the latter collides with util-linux. */
   const char *candidates[] = {home_ast[0] ? home_ast : NULL, home_sg[0] ? home_sg : NULL,
                               "ast-grep", "sg", NULL};
   for (int i = 0; candidates[i]; i++)
   {
      if (strchr(candidates[i], '/') && access(candidates[i], X_OK) != 0)
         continue; /* an explicit path that is not there */
      if (!is_ast_grep(candidates[i]))
         continue;
      snprintf(path, sizeof(path), "%s", candidates[i]);
      return path;
   }
   path[0] = '\0';
   return NULL;
}

/* Expose resolution so the tool surface can withhold ast_grep_search when no
 * ast-grep exists. Advertising a search that cannot run is the same defect as
 * advertising delegate tools with delegation off: the agent spends a call to
 * learn the capability is absent, and before the resolver was fixed it did not
 * even learn that -- it was told there were no matches. */
int ast_grep_available(void)
{
   return ast_grep_binary() != NULL;
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
   if (!sg)
      return text_content(
          "error: ast-grep is not installed (a `sg` on PATH that is util-linux's "
          "set-group-ID command does not count, and is why this used to answer 'No matches "
          "found' instead). Install the release asset for your platform from "
          "https://github.com/ast-grep/ast-grep/releases/latest into ~/.local/bin "
          "(x86_64 Linux: app-x86_64-unknown-linux-gnu.zip, which unzips to "
          "ast-grep and sg)");
   /* --json=stream, NOT bare --json. The parser below is line-oriented (it takes
    * each line starting with '{' as one match), and bare --json emits a single
    * PRETTY-PRINTED array: indented lines, none of which start with '{'. So a
    * search that matched dozens of times parsed to zero and answered
    * "No matches found." -- the same lie this tool told when the binary was
    * missing entirely, and when the binary was util-linux's `sg`.
    *
    * Verified on ast-grep 0.45.1: `--json` gives "[\n  {\n    \"text\": ...",
    * `--json=stream` gives one complete JSON object per line. */
   const char *argv[] = {sg, "--json=stream", "--pattern", pattern, "--lang", lang, path, NULL};

   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, AST_GREP_MAX_OUTPUT);

   /* exit code 127 from execvp means binary not found */
   if (rc == 127 || (!output && rc != 0))
   {
      free(output);
      return text_content("error: ast-grep failed to run. Install the release asset for "
                          "your platform from "
                          "https://github.com/ast-grep/ast-grep/releases/latest into "
                          "~/.local/bin");
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

   int had_output = output && output[0] != '\0';
   free(output);

   /* "No matches found" MUST mean the search ran and matched nothing -- never
    * that we could not read the answer.
    *
    * This tool has now answered that sentence wrongly three separate times: when
    * the resolved binary was util-linux's `sg`, when no ast-grep was installed at
    * all, and when ast-grep's --json emitted a pretty-printed array the
    * line-oriented parser below could not read. Each time it looked exactly like
    * a clean negative result, and an agent asking "does this pattern repeat
    * anywhere" was told no, authoritatively, on a search that never happened.
    *
    * ast-grep with --json=stream prints NOTHING when there are no matches. So
    * non-empty output that parses to zero matches is not a negative result, it
    * is a format we do not understand -- and saying so is the difference between
    * a bug that gets fixed and a bug that gets believed. */
   if (match_count == 0 && had_output)
      return text_content(
          "error: ast-grep produced output this tool could not parse -- expected one JSON "
          "object per line (--json=stream). This is a format mismatch, NOT an empty result: "
          "do not read it as 'no matches'.");

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
