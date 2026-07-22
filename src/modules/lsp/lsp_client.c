/* lsp_client.c: LSP JSON-RPC framing and protocol helpers.
 *
 * Implements the Content-Length framing used by the Language Server Protocol
 * (https://microsoft.github.io/language-server-protocol/specifications/base/0.9/specification/)
 * and provides helpers for parsing and rendering LSP messages.
 *
 * Layer 0 (Foundation) — depends only on cJSON and standard C.
 * No agent or command layer includes permitted here.
 */
#include "aimee.h"
#include "lsp.h"
#include "cJSON.h"
#include <errno.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * Severity label
 * ----------------------------------------------------------------------- */

const char *lsp_severity_label(lsp_severity_t sev)
{
   switch (sev)
   {
   case LSP_SEV_ERROR:
      return "error";
   case LSP_SEV_WARNING:
      return "warning";
   case LSP_SEV_INFO:
      return "info";
   case LSP_SEV_HINT:
      return "hint";
   }
   return "unknown";
}

/* -----------------------------------------------------------------------
 * Content-Length framing
 * ----------------------------------------------------------------------- */

int lsp_frame_write(int fd, const char *json)
{
   if (!json)
      return -1;

   size_t body_len = strlen(json);

   /* "Content-Length: <n>\r\n\r\n<body>" */
   char header[64];
   int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", body_len);
   if (hlen < 0 || (size_t)hlen >= sizeof(header))
      return -1;

   /* Write header */
   const char *p = header;
   size_t remaining = (size_t)hlen;
   while (remaining > 0)
   {
      ssize_t n = write(fd, p, remaining);
      if (n <= 0)
         return -1;
      p += n;
      remaining -= (size_t)n;
   }

   /* Write body */
   p = json;
   remaining = body_len;
   while (remaining > 0)
   {
      ssize_t n = write(fd, p, remaining);
      if (n <= 0)
         return -1;
      p += n;
      remaining -= (size_t)n;
   }

   return 0;
}

int lsp_frame_read(int fd, char *buf, size_t buf_size)
{
   if (!buf || buf_size == 0)
      return -1;

   /* Read header lines until we see the blank separator line */
   size_t content_length = 0;
   int saw_content_length = 0;

   char header_buf[256];
   int header_pos = 0;
   char prev = 0;

   /* Read byte-by-byte to find headers ending in \r\n\r\n */
   while (1)
   {
      char c;
      ssize_t n = read(fd, &c, 1);
      if (n <= 0)
         return -1; /* EOF or error */

      if (header_pos < (int)sizeof(header_buf) - 1)
         header_buf[header_pos++] = c;

      /* Detect end of a header line (\r\n or \n) */
      if (c == '\n')
      {
         header_buf[header_pos] = '\0';

         /* Blank line signals end of headers */
         if (header_pos <= 2) /* "\r\n" or "\n" */
         {
            if (!saw_content_length)
               return -1;
            break;
         }

         /* Parse Content-Length header (case-insensitive) */
         if (strncasecmp(header_buf, "Content-Length:", 15) == 0)
         {
            char *p = header_buf + 15;
            while (*p == ' ' || *p == '\t')
               p++;
            char *end = NULL;
            unsigned long len = strtoul(p, &end, 10);
            if (end && end != p && len > 0)
            {
               content_length = len;
               saw_content_length = 1;
            }
         }

         header_pos = 0;
      }
      (void)prev;
      prev = c;
   }

   /* Read exactly content_length bytes */
   if (content_length >= buf_size)
      return -2; /* buffer too small */

   size_t received = 0;
   while (received < content_length)
   {
      ssize_t n = read(fd, buf + received, content_length - received);
      if (n <= 0)
         return -1;
      received += (size_t)n;
   }

   buf[received] = '\0';
   return (int)received;
}

/* -----------------------------------------------------------------------
 * Diagnostic parsing
 * ----------------------------------------------------------------------- */

/*
 * Collapse embedded newlines in a diagnostic message string into spaces.
 * Writes at most |out_size| - 1 chars.
 */
static void collapse_newlines(const char *src, char *out, size_t out_size)
{
   size_t i = 0;
   for (; *src && i < out_size - 1; src++)
   {
      char c = *src;
      if (c == '\n' || c == '\r')
         out[i++] = ' ';
      else
         out[i++] = c;
   }
   out[i] = '\0';
}

int lsp_parse_diagnostics(const char *json, const char *file_path, lsp_diag_t *out, int max)
{
   if (!json || !out || max <= 0)
      return 0;

   cJSON *root = cJSON_Parse(json);
   if (!root)
      return 0;

   int count = 0;

   /* Navigate: params.diagnostics or diagnostics directly */
   cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
   cJSON *diags_arr = NULL;

   if (params)
   {
      /* Full notification: {"method":"textDocument/publishDiagnostics","params":{...}} */
      diags_arr = cJSON_GetObjectItemCaseSensitive(params, "diagnostics");
      /* If file_path not provided, try to get from params.uri */
      if (!file_path || !file_path[0])
         file_path = NULL; /* will use empty string below */
   }
   else
   {
      /* Bare diagnostics array */
      diags_arr = cJSON_GetObjectItemCaseSensitive(root, "diagnostics");
   }

   if (!cJSON_IsArray(diags_arr))
   {
      cJSON_Delete(root);
      return 0;
   }

   const cJSON *d;
   cJSON_ArrayForEach(d, diags_arr)
   {
      if (count >= max)
         break;

      lsp_diag_t *entry = &out[count];
      memset(entry, 0, sizeof(*entry));

      if (file_path)
         snprintf(entry->file, sizeof(entry->file), "%s", file_path);

      /* Range: range.start.{line,character} */
      cJSON *range = cJSON_GetObjectItemCaseSensitive(d, "range");
      if (cJSON_IsObject(range))
      {
         cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
         if (cJSON_IsObject(start))
         {
            cJSON *ln = cJSON_GetObjectItemCaseSensitive(start, "line");
            cJSON *ch = cJSON_GetObjectItemCaseSensitive(start, "character");
            if (cJSON_IsNumber(ln))
               entry->line = (int)ln->valuedouble;
            if (cJSON_IsNumber(ch))
               entry->col = (int)ch->valuedouble;
         }
      }

      /* Severity */
      cJSON *sev = cJSON_GetObjectItemCaseSensitive(d, "severity");
      if (cJSON_IsNumber(sev))
      {
         int s = (int)sev->valuedouble;
         if (s >= 1 && s <= 4)
            entry->severity = (lsp_severity_t)s;
         else
            entry->severity = LSP_SEV_ERROR;
      }
      else
      {
         entry->severity = LSP_SEV_ERROR;
      }

      /* Message */
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(d, "message");
      if (cJSON_IsString(msg))
         collapse_newlines(msg->valuestring, entry->message, sizeof(entry->message));

      count++;
   }

   cJSON_Delete(root);
   return count;
}

/* -----------------------------------------------------------------------
 * Prompt rendering
 * ----------------------------------------------------------------------- */

int lsp_render_context(const char *focus_file, const lsp_diag_t *diags, int ndiags,
                       const lsp_location_t *defs, int ndefs, const lsp_location_t *refs, int nrefs,
                       char *out, size_t out_size)
{
   if (!out || out_size == 0)
      return 0;

   /* Cap entries */
   if (ndiags > LSP_RENDER_MAX_DIAG)
      ndiags = LSP_RENDER_MAX_DIAG;
   if (ndefs > LSP_RENDER_MAX_SYM)
      ndefs = LSP_RENDER_MAX_SYM;
   if (nrefs > LSP_RENDER_MAX_SYM)
      nrefs = LSP_RENDER_MAX_SYM;

   if (ndiags == 0 && ndefs == 0 && nrefs == 0)
   {
      out[0] = '\0';
      return 0;
   }

   char *p = out;
   size_t left = out_size;

#define APPEND(...)                                                                                \
   do                                                                                              \
   {                                                                                               \
      int _n = snprintf(p, left, __VA_ARGS__);                                                     \
      if (_n > 0 && (size_t)_n < left)                                                             \
      {                                                                                            \
         p += _n;                                                                                  \
         left -= (size_t)_n;                                                                       \
      }                                                                                            \
   } while (0)

   /* Header */
   if (focus_file)
      APPEND("# LSP context\nFocus: %s", focus_file);
   else
      APPEND("# LSP context");

   if (ndiags > 0)
      APPEND(" — %d diagnostic%s across workspace", ndiags, ndiags == 1 ? "" : "s");
   APPEND("\n\n");

   /* Diagnostics */
   if (ndiags > 0)
   {
      APPEND("Diagnostics (showing %d of %d):\n", ndiags, ndiags);
      for (int i = 0; i < ndiags; i++)
      {
         const lsp_diag_t *d = &diags[i];
         APPEND(" - %s:%d:%d [%s] %s\n", d->file[0] ? d->file : "(unknown)", d->line + 1,
                d->col + 1, lsp_severity_label(d->severity), d->message);
      }
      APPEND("\n");
   }

   /* Definitions */
   if (ndefs > 0)
   {
      APPEND("Definitions (showing %d of %d):\n", ndefs, ndefs);
      for (int i = 0; i < ndefs; i++)
      {
         const lsp_location_t *loc = &defs[i];
         APPEND(" - %s: %s:%d\n", loc->symbol[0] ? loc->symbol : "(unknown)",
                loc->file[0] ? loc->file : "(unknown)", loc->line + 1);
      }
      APPEND("\n");
   }

   /* References */
   if (nrefs > 0)
   {
      APPEND("References (showing %d of %d):\n", nrefs, nrefs);
      for (int i = 0; i < nrefs; i++)
      {
         const lsp_location_t *loc = &refs[i];
         APPEND(" - %s: %s:%d\n", loc->symbol[0] ? loc->symbol : "(unknown)",
                loc->file[0] ? loc->file : "(unknown)", loc->line + 1);
      }
   }

#undef APPEND

   return (int)(out_size - left);
}
