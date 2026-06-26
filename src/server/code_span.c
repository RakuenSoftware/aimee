/* code_span.c: see code_span.h. The bounded, validated source-span read behind
 * the `code_span_get` MCP resolver. */
#include "code_span.h"
#include "aimee.h" /* MAX_PATH_LEN, base types guardrails.h relies on */
#include "dstr.h"
#include "guardrails.h"
#include "kb_doc_hash.h"
#include "workspace_provider.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Hard byte cap on the returned span, independent of the line clamp: a handful
 * of pathologically long lines must not blow the envelope/recovery budget. */
#define CODE_SPAN_MAX_BYTES (64 * 1024)

static cJSON *span_err(const char *msg)
{
   cJSON *o = cJSON_CreateObject();
   if (o)
      cJSON_AddStringToObject(o, "error", msg);
   return o;
}

/* Reject control chars (incl. would-be NUL framing tricks and DEL). */
static int has_ctrl_chars(const char *s)
{
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (*p < 0x20 || *p == 0x7f)
         return 1;
   return 0;
}

/* Is the realpath `path` inside (or equal to) realpath `root`? Both absolute. */
static int path_within_root(const char *path, const char *root)
{
   size_t len = strlen(root);
   while (len > 1 && root[len - 1] == '/') /* normalize a trailing slash */
      len--;
   if (len == 0)
      return 0;
   if (strncmp(path, root, len) != 0)
      return 0;
   return path[len] == '/' || path[len] == '\0';
}

cJSON *code_span_read(const char *project, const char *project_root, const char *file_path,
                      int line_start, int line_end, int max_lines)
{
   if (!file_path || !file_path[0])
      return span_err("file_path is required");
   if (!project_root || project_root[0] != '/')
      return span_err("project root is not an absolute path");
   if (has_ctrl_chars(file_path))
      return span_err("file_path contains control characters");

   if (max_lines <= 0)
      max_lines = 400;
   if (line_start < 1)
      line_start = 1;
   if (line_end < line_start)
      line_end = line_start;
   /* Clamp the requested span to max_lines, but only *report* truncation if the
    * file actually had content past the clamped end (decided after the read) —
    * a [1,1000] request on a 5-line file is not truncated. */
   int clamped_by_max = 0;
   if (line_end - line_start + 1 > max_lines)
   {
      line_end = line_start + max_lines - 1;
      clamped_by_max = 1;
   }
   int truncated = 0;

   /* Build the candidate path. A relative file_path joins the project root; an
    * absolute file_path is taken as-is but must still resolve back inside root. */
   char candidate[MAX_PATH_LEN];
   if (file_path[0] == '/')
   {
      if ((size_t)snprintf(candidate, sizeof(candidate), "%s", file_path) >= sizeof(candidate))
         return span_err("file_path too long");
   }
   else if ((size_t)snprintf(candidate, sizeof(candidate), "%s/%s", project_root, file_path) >=
            sizeof(candidate))
   {
      return span_err("file_path too long");
   }

   /* realpath + `..`/symlink-escape rejection + sensitive-path deny-list. */
   char resolved[MAX_PATH_LEN];
   const char *verr = guardrails_validate_file_path(candidate, resolved, sizeof(resolved));
   if (verr)
      return span_err(verr);

   /* Containment: the realpath must stay within the project root's realpath. */
   char root_real[MAX_PATH_LEN];
   if (!realpath(project_root, root_real))
   {
      if ((size_t)snprintf(root_real, sizeof(root_real), "%s", project_root) >= sizeof(root_real))
         return span_err("project root too long");
   }
   if (!path_within_root(resolved, root_real))
      return span_err("path resolves outside the project workspace");

   /* Pull bytes through the active workspace provider (never open directly).
    * Pass the realpath-resolved path so a symlink swap after validation cannot
    * redirect the read (we read the canonical path we already contained). */
   const workspace_provider_t *ws = workspace_provider_active();
   char *data = NULL;
   size_t len = 0;
   if (!ws || !ws->read_all || ws->read_all(ws, resolved, &data, &len) != 0)
      return span_err("cannot read source file");

   /* source_version is the WHOLE-FILE content hash, not the slice hash: it is the
    * §1.1 drift signal for "has the source changed since the fold?", which must
    * fire for any file mutation (incl. edits outside the returned span that shift
    * line numbers). This matches the file-level content_hash the code index
    * already records on a hit. */
   char ver[KB_DOC_HASH_HEX_LEN + 1];
   kb_doc_content_hash(data, (int)len, ver);

   /* Slice [line_start, line_end], newline-inclusive, under the byte cap. */
   dstr_t out;
   dstr_init(&out);
   int line_no = 0, emitted = 0;
   size_t i = 0;
   while (i < len && line_no < line_end)
   {
      size_t start = i;
      while (i < len && data[i] != '\n')
         i++;
      size_t eol = (i < len) ? i + 1 : i; /* include the newline when present */
      line_no++;
      if (line_no >= line_start)
      {
         size_t seg = eol - start;
         if (dstr_len(&out) + seg > CODE_SPAN_MAX_BYTES)
         {
            truncated = 1;
            break;
         }
         dstr_append(&out, data + start, seg);
         emitted++;
      }
      i = eol;
   }
   /* The line clamp truncated only if unconsumed file bytes remain past where we
    * stopped (i < len): otherwise we returned everything the request covered. */
   if (clamped_by_max && i < len)
      truncated = 1;
   free(data);

   cJSON *o = cJSON_CreateObject();
   if (!o)
   {
      dstr_free(&out);
      return NULL;
   }
   if (project && project[0])
      cJSON_AddStringToObject(o, "project", project);
   cJSON_AddStringToObject(o, "file_path", file_path);
   cJSON_AddNumberToObject(o, "line_start", line_start);
   cJSON_AddNumberToObject(o, "line_end", emitted > 0 ? line_start + emitted - 1 : 0);
   cJSON_AddNumberToObject(o, "line_count", emitted);
   cJSON_AddBoolToObject(o, "truncated", truncated);
   cJSON_AddStringToObject(o, "content", dstr_cstr(&out));
   cJSON_AddStringToObject(o, "source_version", ver);
   dstr_free(&out);
   return o;
}
