/* code_outline.c: anchored source-structure views (pure).
 * (proposal: hashline-edit-and-lean-websearch, Part III.) */
#include "code_outline.h"
#include "anchor_snapshot.h"
#include "dstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTLINE_MAX_DEFS 512

const char *code_outline_ext(const char *path)
{
   if (!path)
      return "";
   const char *dot = strrchr(path, '.');
   if (!dot || dot == path)
      return "";
   return dot;
}

char *code_outline_format(const char *content, size_t len, const char *ext, const char *snapshot_id)
{
   definition_t *defs = malloc(OUTLINE_MAX_DEFS * sizeof(*defs));
   if (!defs)
      return NULL;
   int n = extract_definitions(ext && ext[0] ? ext : "", content, defs, OUTLINE_MAX_DEFS);

   dstr_t ds;
   dstr_init(&ds);
   if (snapshot_id && snapshot_id[0])
   {
      dstr_append_str(&ds, "# outline snapshot=");
      dstr_append_str(&ds, snapshot_id);
      dstr_append_str(&ds, " (read/edit a symbol by its LINE:HASH anchor)\n");
   }
   if (n <= 0)
   {
      dstr_append_str(&ds,
                      "(no symbol outline available for this file type; read it with raw:false)\n");
      free(defs);
      char *out = dstr_steal(&ds);
      if (!out)
      {
         dstr_free(&ds);
         out = calloc(1, 1);
      }
      return out;
   }

   /* pre-split once for digests */
   anchor_line_t *lines = NULL;
   int lc = anchor_split_lines(content, len, &lines);
   for (int i = 0; i < n; i++)
   {
      int ln = defs[i].line;
      uint64_t d = (ln >= 1 && ln <= lc)
                       ? anchor_line_digest(lines[ln - 1].ptr, lines[ln - 1].len, ln == 1)
                       : 0;
      char tag[3];
      anchor_short_tag(d, tag);
      char row[256];
      if (defs[i].line_end > defs[i].line)
         snprintf(row, sizeof(row), "%d:%s| %s %s  (lines %d-%d)\n", ln, tag, defs[i].kind,
                  defs[i].name, defs[i].line, defs[i].line_end);
      else
         snprintf(row, sizeof(row), "%d:%s| %s %s  (line %d)\n", ln, tag, defs[i].kind,
                  defs[i].name, defs[i].line);
      dstr_append_str(&ds, row);
   }
   free(lines);
   free(defs);
   char *out = dstr_steal(&ds);
   if (!out)
   {
      dstr_free(&ds);
      out = calloc(1, 1);
   }
   return out;
}

int code_outline_symbol_defs(const char *content, size_t len, const char *ext, const char *symbol,
                             definition_t *out, int max)
{
   (void)len;
   if (!symbol || !symbol[0] || max <= 0)
      return -1;
   definition_t *defs = malloc(OUTLINE_MAX_DEFS * sizeof(*defs));
   if (!defs)
      return -1;
   int n = extract_definitions(ext && ext[0] ? ext : "", content, defs, OUTLINE_MAX_DEFS);
   int found = 0;
   for (int i = 0; i < n && found < max; i++)
   {
      if (strcmp(defs[i].name, symbol) == 0)
         out[found++] = defs[i];
   }
   free(defs);
   return found;
}
