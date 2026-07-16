/* posix/agent_tools_anchored.c: Part III agent-shaped tools that build on the
 * Part I anchor engine + the existing condense/spill contract.
 * (proposal: hashline-edit-and-lean-websearch, Part III.)
 *
 *   read_file mode:"outline"  -> anchored symbol skeleton (code_outline)
 *   read_symbol               -> one symbol's def span, anchored
 *   grep anchored:true        -> hits as ready edit anchors (per-file snapshot)
 *   run_tests                 -> counts + failures only, full log spilled
 *
 * File I/O routes through the workspace provider; path policy through the same
 * public guardrail as the other tools. */
#include "aimee.h"
#include "agent_tools.h"
#include "agent_tools_internal.h"
#include "anchor_snapshot.h"
#include "code_outline.h"
#include "guardrails.h"
#include "kb_client.h"
#include "tool_condense.h"
#include "aimee_home.h"
#include "config.h"
#include "workspace_provider.h"
#include "dstr.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read a workspace file into a malloc'd buffer with a cheap binary guard.
 * Returns 0 and sets out+len on success; -1 (and an error string via *errmsg,
 * a static literal) otherwise. */
static int read_text_file(const char *actual_path, char **out, size_t *len, const char **errmsg)
{
   const workspace_provider_t *ws = workspace_provider_active();
   char *data = NULL;
   size_t n = 0;
   if (ws->read_all(ws, actual_path, &data, &n) != 0)
   {
      *errmsg = "error: cannot open file";
      return -1;
   }
   size_t scan = n < 4096 ? n : 4096;
   for (size_t i = 0; i < scan; i++)
   {
      if (data[i] == '\0')
      {
         free(data);
         *errmsg = "error: binary file omitted";
         return -1;
      }
   }
   *out = data;
   *len = n;
   return 0;
}

char *tool_read_outline(const char *path)
{
   if (!path || !path[0])
      return safe_strdup("error: missing 'path' parameter");
   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   char resolved[MAX_PATH_LEN];
   const char *verr = guardrails_validate_file_path(actual_path, resolved, sizeof(resolved));
   if (verr)
      return safe_strdup(verr);

   char *data = NULL;
   size_t len = 0;
   const char *errmsg = NULL;
   if (read_text_file(actual_path, &data, &len, &errmsg) != 0)
      return safe_strdup(errmsg);

   char snap[ANCHOR_SNAPSHOT_ID_MAX];
   if (anchor_snapshot_create(resolved, data, len, snap) != 0)
      snap[0] = '\0';
   char *out = code_outline_format(data, len, code_outline_ext(actual_path), snap[0] ? snap : NULL);
   free(data);
   return out ? out : safe_strdup("error: out of memory");
}

/* Emit the def span [start..end] of a resolved symbol, anchored + headed. */
static char *format_symbol_span(const char *resolved, const char *actual_path, char *data,
                                size_t len, const char *symbol, int start, int end)
{
   char snap[ANCHOR_SNAPSHOT_ID_MAX];
   if (anchor_snapshot_create(resolved, data, len, snap) != 0)
      snap[0] = '\0';
   char *span = anchor_format_read(data, len, start - 1, end - start + 1, snap[0] ? snap : NULL);
   if (!span)
      return safe_strdup("error: out of memory");
   dstr_t ds;
   dstr_init(&ds);
   char hdr[MAX_PATH_LEN + 128];
   snprintf(hdr, sizeof(hdr), "# %s  %s:%d-%d\n", symbol, actual_path, start, end);
   dstr_append_str(&ds, hdr);
   dstr_append_str(&ds, span);
   free(span);
   char *out = dstr_steal(&ds);
   if (!out)
   {
      dstr_free(&ds);
      out = safe_strdup("error: out of memory");
   }
   return out;
}

/* List multiple same-named defs as anchored outline rows for disambiguation. */
static char *format_symbol_candidates(char *data, size_t len, const char *snap,
                                      const definition_t *defs, int n)
{
   dstr_t ds;
   dstr_init(&ds);
   dstr_append_str(&ds, "# ambiguous symbol: choose a candidate (pass a path, or read the span)\n");
   anchor_line_t *lines = NULL;
   int lc = anchor_split_lines(data, len, &lines);
   for (int i = 0; i < n; i++)
   {
      int ln = defs[i].line;
      uint64_t d = (ln >= 1 && ln <= lc)
                       ? anchor_line_digest(lines[ln - 1].ptr, lines[ln - 1].len, ln == 1)
                       : 0;
      char tag[3];
      anchor_short_tag(d, tag);
      char row[256];
      snprintf(row, sizeof(row), "%d:%s| %s %s  (lines %d-%d)\n", ln, tag, defs[i].kind,
               defs[i].name, defs[i].line,
               defs[i].line_end > defs[i].line ? defs[i].line_end : defs[i].line);
      dstr_append_str(&ds, row);
   }
   free(lines);
   (void)snap;
   char *out = dstr_steal(&ds);
   if (!out)
   {
      dstr_free(&ds);
      out = safe_strdup("error: out of memory");
   }
   return out;
}

char *tool_read_symbol(const char *symbol, const char *path)
{
   if (!symbol || !symbol[0])
      return safe_strdup("error: missing 'symbol' parameter");

   /* Resolve which file to read: an explicit path, or the code index. */
   char resolved_path[MAX_PATH_LEN];
   const char *target = NULL;
   int idx_start = 0, idx_end = 0;
   if (path && path[0])
   {
      target = path;
   }
   else
   {
      term_hit_t hits[16];
      int nh = kb_client_index_find(symbol, hits, 16);
      if (nh <= 0)
         return safe_strdup("error: symbol not found in the code index; pass 'path' to read from a "
                            "specific file");
      if (nh > 1)
      {
         /* index-level ambiguity: list locations, require a path or FQN */
         dstr_t ds;
         dstr_init(&ds);
         dstr_append_str(&ds, "# ambiguous symbol across files; pass 'path' to disambiguate\n");
         for (int i = 0; i < nh; i++)
         {
            char row[MAX_PATH_LEN + 64];
            snprintf(row, sizeof(row), "- %s:%d [%s]\n", hits[i].file_path, hits[i].line,
                     hits[i].kind);
            dstr_append_str(&ds, row);
         }
         char *out = dstr_steal(&ds);
         return out ? out : safe_strdup("error: out of memory");
      }
      target = hits[0].file_path;
      idx_start = hits[0].line;
      idx_end = hits[0].line_end > hits[0].line ? hits[0].line_end : hits[0].line;
   }

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(target, cwd_path, sizeof(cwd_path));
   const char *verr =
       guardrails_validate_file_path(actual_path, resolved_path, sizeof(resolved_path));
   if (verr)
      return safe_strdup(verr);

   char *data = NULL;
   size_t len = 0;
   const char *errmsg = NULL;
   if (read_text_file(actual_path, &data, &len, &errmsg) != 0)
      return safe_strdup(errmsg);

   /* Prefer extracting the span from the file itself (works offline). */
   definition_t defs[16];
   int n = code_outline_symbol_defs(data, len, code_outline_ext(actual_path), symbol, defs, 16);
   if (n > 1)
   {
      char *out = format_symbol_candidates(data, len, NULL, defs, n);
      free(data);
      return out;
   }
   int start = 0, end = 0;
   if (n == 1)
   {
      start = defs[0].line;
      end = defs[0].line_end > defs[0].line ? defs[0].line_end : defs[0].line;
   }
   else if (idx_start > 0)
   {
      /* fall back to the index's span when the file extractor found nothing */
      start = idx_start;
      end = idx_end;
   }
   else
   {
      free(data);
      return safe_strdup("error: symbol not found in file");
   }
   char *out = format_symbol_span(resolved_path, actual_path, data, len, symbol, start, end);
   free(data);
   return out;
}

/* Anchored grep: reuse tool_grep's raw "path:line:text" output, then group by
 * file, mint one snapshot per file, and re-emit each hit as an editable
 * "line:HASH| text" row under a "path  snapshot=sID" header. */
char *tool_grep_anchored(const char *path, const char *pattern, int max_results)
{
   char *raw = tool_grep(path, pattern, max_results);
   if (!raw)
      return safe_strdup("no matches found");
   if (strncmp(raw, "error:", 6) == 0 || strncmp(raw, "no matches", 10) == 0)
      return raw;

   dstr_t ds;
   dstr_init(&ds);
   char last_file[MAX_PATH_LEN] = {0};
   char *file_data = NULL;
   size_t file_len = 0;
   anchor_line_t *file_lines = NULL;
   int file_lc = 0;

   char *save = NULL;
   for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      /* parse "path:lineno:text" */
      char *c1 = strchr(line, ':');
      if (!c1)
         continue;
      char *c2 = strchr(c1 + 1, ':');
      if (!c2)
         continue;
      *c1 = '\0';
      *c2 = '\0';
      const char *fpath = line;
      int lineno = atoi(c1 + 1);
      const char *text = c2 + 1;

      if (strcmp(fpath, last_file) != 0)
      {
         /* new file: read + mint a snapshot */
         free(file_data);
         free(file_lines);
         file_data = NULL;
         file_lines = NULL;
         file_lc = 0;
         char cwd_path[MAX_PATH_LEN];
         const char *actual = path_in_thread_cwd(fpath, cwd_path, sizeof(cwd_path));
         char resolved[MAX_PATH_LEN];
         const char *verr = guardrails_validate_file_path(actual, resolved, sizeof(resolved));
         char snap[ANCHOR_SNAPSHOT_ID_MAX] = {0};
         const char *errmsg = NULL;
         if (!verr && read_text_file(actual, &file_data, &file_len, &errmsg) == 0)
         {
            if (anchor_snapshot_create(resolved, file_data, file_len, snap) != 0)
               snap[0] = '\0';
            file_lc = anchor_split_lines(file_data, file_len, &file_lines);
         }
         snprintf(last_file, sizeof(last_file), "%s", fpath);
         char hdr[MAX_PATH_LEN + 64];
         if (snap[0])
            snprintf(hdr, sizeof(hdr), "%s  snapshot=%s\n", fpath, snap);
         else
            snprintf(hdr, sizeof(hdr), "%s\n", fpath);
         dstr_append_str(&ds, hdr);
      }

      char tag[3] = "00";
      if (file_lines && lineno >= 1 && lineno <= file_lc)
         anchor_short_tag(anchor_line_digest(file_lines[lineno - 1].ptr, file_lines[lineno - 1].len,
                                             lineno == 1),
                          tag);
      char row[256];
      snprintf(row, sizeof(row), "  %d:%s| ", lineno, tag);
      dstr_append_str(&ds, row);
      dstr_append_str(&ds, text);
      dstr_append_str(&ds, "\n");
   }
   free(file_data);
   free(file_lines);
   free(raw);
   char *out = dstr_steal(&ds);
   if (!out)
   {
      dstr_free(&ds);
      out = safe_strdup("no matches found");
   }
   return out;
}

char *tool_run_tests(const char *command, int timeout_ms)
{
   if (!command || !command[0])
      return safe_strdup("error: missing 'command' parameter");

   char *bash_out = tool_bash(command, timeout_ms);
   if (!bash_out)
      return safe_strdup("error: test command failed to run");

   cJSON *bj = cJSON_Parse(bash_out);
   if (!bj)
      return bash_out; /* pass through if it wasn't the expected JSON */

   cJSON *so = cJSON_GetObjectItem(bj, "stdout");
   cJSON *se = cJSON_GetObjectItem(bj, "stderr");
   cJSON *ec = cJSON_GetObjectItem(bj, "exit_code");
   int exit_code = (ec && cJSON_IsNumber(ec)) ? ec->valueint : -1;

   dstr_t raw;
   dstr_init(&raw);
   if (so && cJSON_IsString(so))
      dstr_append_str(&raw, so->valuestring);
   if (se && cJSON_IsString(se) && se->valuestring[0])
   {
      dstr_append_str(&raw, "\n");
      dstr_append_str(&raw, se->valuestring);
   }

   /* Condense: keep the framework summary + every failure, spill the full log.
    * Reuses the command-aware condenser (test-runner family) + spill store. */
   config_t cfg;
   config_load(&cfg);
   char spill_dir[600];
   const char *home = aimee_home();
   const char *sd = NULL;
   if (home && home[0] &&
       snprintf(spill_dir, sizeof(spill_dir), "%s/tool-spills", home) < (int)sizeof(spill_dir))
      sd = spill_dir;
   char *condensed =
       tool_condense_apply(&cfg, command, exit_code, raw.data ? raw.data : "", sd, NULL);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "status", exit_code == 0 ? "passed" : "failed");
   cJSON_AddBoolToObject(out, "passed", exit_code == 0);
   cJSON_AddNumberToObject(out, "exit_code", exit_code);
   cJSON_AddStringToObject(out, "output", condensed ? condensed : (raw.data ? raw.data : ""));
   free(condensed);
   dstr_free(&raw);
   cJSON_Delete(bj);
   free(bash_out);
   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   return s ? s : safe_strdup("error: out of memory");
}
