/* posix/agent_tools_outline.c: read_file outline mode.
 *
 * Returns a file's symbol skeleton — one anchored signature line per top-level
 * definition (function/type/macro), no bodies — so one cheap call maps a large
 * file and the agent can then read/edit one span by its N:hash anchor instead of
 * paging the whole file. Reuses the code extractor (tree-sitter with a hand-rolled
 * fallback) + the hashline snapshot/anchor contract. */
#include "aimee.h"
#include "agent_tools.h"
#include "agent_tools_internal.h" /* path_in_thread_cwd */
#include "agent_exec.h"           /* agent_tool_output_cap */
#include "guardrails.h"           /* guardrails_validate_file_path */
#include "hashline_anchor.h"
#include "index.h" /* definition_t, extract_definitions */
#include "util.h"
#include "workspace_provider.h"

#include "dstr.h"

#include <stdlib.h>
#include <string.h>

static int outline_cmp_by_line(const void *a, const void *b)
{
   int la = ((const definition_t *)a)->line;
   int lb = ((const definition_t *)b)->line;
   return (la > lb) - (la < lb);
}

#define OUTLINE_MAX_DEFS 512

char *tool_read_file_outline(const char *path, const char *sid)
{
   if (!path || !path[0])
      return safe_strdup("error: missing 'path' parameter");

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));
   char resolved[MAX_PATH_LEN];
   const char *verr = guardrails_validate_file_path(actual_path, resolved, sizeof(resolved));
   if (verr)
      return safe_strdup(verr);

   const workspace_provider_t *ws = workspace_provider_active();
   char *content = NULL;
   size_t len = 0;
   if (ws->read_all(ws, actual_path, &content, &len) != 0)
   {
      char e[512];
      snprintf(e, sizeof(e), "error: cannot open %s", actual_path);
      return safe_strdup(e);
   }

   const char *ext = strrchr(path, '.');
   if (!ext)
      ext = "";
   definition_t *defs = calloc(OUTLINE_MAX_DEFS, sizeof(definition_t));
   if (!defs)
   {
      free(content);
      return safe_strdup("error: out of memory");
   }
   int ndefs = extract_definitions(ext, content, defs, OUTLINE_MAX_DEFS);
   if (ndefs <= 0)
   {
      free(defs);
      free(content);
      return safe_strdup("outline: no top-level definitions found (unsupported file type or no "
                         "symbols) — use read_file to view the contents");
   }
   qsort(defs, (size_t)ndefs, sizeof(definition_t), outline_cmp_by_line);

   char *snap = hashline_snapshot_mint(sid, actual_path, content, len);
   dstr_t out;
   dstr_init(&out);
   if (snap)
      dstr_appendf(&out,
                   "[outline of %s — snapshot %s; %d definitions; edit a signature by its N:hash "
                   "anchor, or read_file its span]\n",
                   actual_path, snap, ndefs);
   else
      dstr_appendf(&out, "[outline of %s — snapshot unavailable; %d definitions]\n", actual_path,
                   ndefs);

   /* Single pass over the file lines; emit each definition's START line (its
    * signature) with the same anchor an anchored read would show. */
   size_t cap = agent_tool_output_cap();
   size_t i = 0;
   int lineno = 0, di = 0;
   int truncated = 0;
   while (i < len && di < ndefs)
   {
      size_t s = i;
      while (i < len && content[i] != '\n')
         i++;
      size_t e = i;
      int has_nl = (i < len);
      if (has_nl)
         i++;
      lineno++;
      while (di < ndefs && defs[di].line < lineno)
         di++; /* skip any def whose line we've passed */
      while (di < ndefs && defs[di].line == lineno)
      {
         if (out.len + (e - s) + 32 >= cap)
         {
            truncated = 1;
            di = ndefs;
            break;
         }
         uint64_t d = hashline_digest64(content + s, e - s, lineno == 1, has_nl);
         char tag[HASHLINE_DISPLAY_TAG_HEX + 1];
         hashline_display_tag(d, tag, sizeof(tag));
         dstr_appendf(&out, "%d:%s| ", lineno, tag);
         dstr_append(&out, content + s, e - s);
         dstr_append_char(&out, '\n');
         di++;
      }
   }
   if (truncated)
      dstr_appendf(&out, "[outline truncated at size cap; read_file a region for the rest]\n");
   else if (di < ndefs)
      /* extract_definitions returns 1-based lines within the parsed content, so
       * this is defensive: any definition the single pass could not place at a
       * file line (e.g. a line beyond EOF) is reported, never silently dropped. */
      dstr_appendf(&out,
                   "[note: %d definition(s) could not be placed at a file line and were omitted; "
                   "read_file to view them]\n",
                   ndefs - di);

   free(snap);
   free(defs);
   free(content);
   return out.data ? out.data : safe_strdup("error: out of memory");
}
