/* posix/agent_tools_grep_anchor.c: anchored-grep post-processor.
 *
 * Wraps tool_grep so that, by default, hits are grouped by file with a per-file
 * read snapshot and each matching line carries its `N:hash` edit anchor — making
 * a grep hit directly editable via edit_file with no intervening read. Split out
 * of posix/agent_tools.c to keep that file under the line-count limit. */
#include "aimee.h"
#include "agent_tools.h"
#include "agent_tools_internal.h"
#include "hashline_anchor.h"
#include "util.h"
#include "workspace_provider.h"

#include "dstr.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Parse one `grep -rn` output line. The output SHAPE is determined by whether the
 * search target was a single file or a directory, so the caller passes
 * `single_file` rather than guessing per line (removing the ambiguity between a
 * "LINENO:CONTENT" single-file line and a directory line whose path starts with
 * digits):
 *   - single_file: "LINENO:CONTENT" — grep omits the filename; `*path` is set
 *     NULL and the caller supplies the searched file path;
 *   - directory:   "PATH:LINENO:CONTENT" — first ":<digits>:" is the LINENO
 *     boundary (LINENO sits right after the path; CONTENT may contain its own
 *     ":<digits>:" runs, so first-match beats last-match). A path containing a
 *     colon can still misparse, but only degrades to a passthrough line (the read
 *     of the misparsed path fails to resolve), never a crash.
 * LINENO is required to be >= 1. Returns 0 on success, -1 if not a grep hit. */
static int grep_parse_line(const char *line, size_t linelen, int single_file, const char **path,
                           size_t *pathlen, long *lineno, const char **content)
{
   if (single_file)
   {
      size_t d = 0;
      while (d < linelen && isdigit((unsigned char)line[d]))
         d++;
      if (d == 0 || d >= linelen || line[d] != ':')
         return -1;
      long v = strtol(line, NULL, 10);
      if (v < 1)
         return -1;
      *path = NULL;
      *pathlen = 0;
      *lineno = v;
      *content = line + d + 1;
      return 0;
   }
   for (size_t i = 0; i < linelen; i++)
   {
      if (line[i] != ':' || i + 1 >= linelen || !isdigit((unsigned char)line[i + 1]))
         continue;
      size_t j = i + 1;
      while (j < linelen && isdigit((unsigned char)line[j]))
         j++;
      if (j < linelen && line[j] == ':')
      {
         long v = strtol(line + i + 1, NULL, 10);
         if (v < 1)
            return -1;
         *path = line;
         *pathlen = i;
         *lineno = v;
         *content = line + j + 1;
         return 0;
      }
   }
   return -1;
}

#define GREP_MAX_FILES 256
typedef struct
{
   char *path;                    /* NUL-terminated file path from grep */
   char *snap;                    /* minted snapshot id (owned) */
   hashline_snapshot_view_t view; /* owned digests (freed at end) */
   int resolved;                  /* 1 if we successfully read+minted */
} grep_file_t;

/* Anchored grep: post-process raw `grep -rn` output so every hit is a ready edit
 * anchor. Groups hits by file, mints one snapshot per file, and prefixes each
 * matching line with its `N:hash` anchor. raw:true (anchored=0) returns the plain
 * grep output. */
char *tool_grep_ex(const char *path, const char *pattern, int max_results, int anchored,
                   const char *sid)
{
   char *raw = tool_grep(path, pattern, max_results);
   if (!anchored || !raw || strncmp(raw, "error:", 6) == 0 || strcmp(raw, "no matches found") == 0)
      return raw;

   grep_file_t files[GREP_MAX_FILES];
   int nfiles = 0;
   dstr_t out;
   dstr_init(&out);

   /* Grep's output shape depends on whether the target is a file or a directory.
    * Resolve the query path once and stat it, so the parser is told the shape
    * definitively rather than guessing per line. */
   char qbuf[MAX_PATH_LEN];
   const char *query_path = path_in_thread_cwd(path, qbuf, sizeof(qbuf));
   struct stat qst;
   int single_file = (stat(query_path, &qst) == 0 && S_ISREG(qst.st_mode));

   const workspace_provider_t *ws = workspace_provider_active();
   size_t rawlen = strlen(raw);
   size_t i = 0;
   const char *last_emitted_path = NULL;
   int cap_hit = 0;
   while (i < rawlen)
   {
      size_t s = i;
      while (i < rawlen && raw[i] != '\n')
         i++;
      size_t e = i;
      if (i < rawlen)
         i++;
      if (e == s)
         continue;

      const char *p = NULL, *content = NULL;
      size_t plen = 0;
      long lineno = 0;
      if (grep_parse_line(raw + s, e - s, single_file, &p, &plen, &lineno, &content) != 0)
      {
         /* Non-matching line (e.g. a grep note) — pass through verbatim. */
         dstr_append(&out, raw + s, e - s);
         dstr_append_char(&out, '\n');
         continue;
      }
      /* Single-file shape (p==NULL): the file is the resolved query path. */
      const char *fp = p ? p : query_path;
      size_t fplen = p ? plen : strlen(query_path);

      /* Find or create the per-file cache entry (read + mint once). Entries in
       * files[] always have a non-NULL path (we only insert on a successful
       * strndup), so the dedup compare below never dereferences NULL. */
      grep_file_t *gf = NULL;
      for (int k = 0; k < nfiles; k++)
         if (strncmp(files[k].path, fp, fplen) == 0 && files[k].path[fplen] == '\0')
         {
            gf = &files[k];
            break;
         }
      if (!gf)
      {
         if (nfiles >= GREP_MAX_FILES)
            cap_hit = 1;
         else
         {
            char *pcopy = strndup(fp, fplen);
            if (pcopy)
            {
               gf = &files[nfiles++];
               memset(gf, 0, sizeof(*gf));
               gf->path = pcopy;
               char *fdata = NULL;
               size_t flen = 0;
               if (ws->read_all(ws, gf->path, &fdata, &flen) == 0)
               {
                  gf->snap = hashline_snapshot_mint(sid, gf->path, fdata, flen);
                  if (gf->snap && hashline_snapshot_get(sid, gf->snap, &gf->view))
                     gf->resolved = 1;
               }
               free(fdata);
            }
         }
      }

      size_t content_len = (size_t)((raw + e) - content);
      const char *pathz = gf ? gf->path : NULL;
      if (!pathz)
      {
         /* Cache full or path copy failed — emit a still-useful hit line. */
         dstr_appendf(&out, "%.*s:%ld: ", (int)fplen, fp, lineno);
         dstr_append(&out, content, content_len);
         dstr_append_char(&out, '\n');
         continue;
      }

      /* Emit a file header once per file (with its snapshot id). grep -rn groups
       * all of a file's hits contiguously, so a path change is a new file. */
      if (!last_emitted_path || strcmp(last_emitted_path, pathz) != 0)
      {
         if (gf->resolved && gf->snap)
            dstr_appendf(&out, "%s  [snapshot %s]\n", pathz, gf->snap);
         else
            dstr_appendf(&out, "%s  [no snapshot — re-read before editing]\n", pathz);
         last_emitted_path = pathz;
      }

      if (gf->resolved && lineno >= 1 && (size_t)lineno <= gf->view.line_count)
      {
         char tag[HASHLINE_DISPLAY_TAG_HEX + 1];
         hashline_display_tag(gf->view.line_digests[lineno - 1], tag, sizeof(tag));
         dstr_appendf(&out, "  %ld:%s| ", lineno, tag);
      }
      else
      {
         /* Unresolved file or a line past EOF: an explicit non-anchor marker so
          * the model never mistakes it for an editable anchor. */
         dstr_appendf(&out, "  %ld:??| ", lineno);
      }
      dstr_append(&out, content, content_len);
      dstr_append_char(&out, '\n');
   }

   if (cap_hit)
      dstr_appendf(&out,
                   "[note: more than %d files matched; hits beyond the first %d files are "
                   "unanchored — narrow the search or grep them individually]\n",
                   GREP_MAX_FILES, GREP_MAX_FILES);

   for (int k = 0; k < nfiles; k++)
   {
      free(files[k].path);
      free(files[k].snap);
      if (files[k].resolved)
         hashline_snapshot_view_free(&files[k].view);
   }
   free(raw);
   char *ret = out.data ? out.data : safe_strdup("no matches found");
   return ret;
}
