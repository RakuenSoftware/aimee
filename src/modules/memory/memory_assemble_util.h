/* memory_assemble_util.h: pure, dependency-free helpers extracted from
 * memory_assemble.c so the context-rendering string logic can be unit-tested
 * in isolation. static inline → no link dependency (the jo_type_name /
 * session_start_util pattern); the companion test links nothing extra. */
#ifndef DEC_MEMORY_ASSEMBLE_UTIL_H
#define DEC_MEMORY_ASSEMBLE_UTIL_H 1

#include <stddef.h>
#include <string.h>

/* XML-escape `src` into `dst` (NUL-terminated, never overflowing dst_len),
 * replacing & < > " with their entities. Stops cleanly at the buffer edge
 * (never emits a partial entity). Tolerates NULL/empty src → dst becomes "". */
static inline void xml_escape_text(const char *src, char *dst, size_t dst_len)
{
   if (!dst || dst_len == 0)
      return;
   dst[0] = '\0';
   if (!src || !src[0])
      return;

   size_t used = 0;
   for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < dst_len; p++)
   {
      const char *rep = NULL;
      switch (*p)
      {
      case '&':
         rep = "&amp;";
         break;
      case '<':
         rep = "&lt;";
         break;
      case '>':
         rep = "&gt;";
         break;
      case '"':
         rep = "&quot;";
         break;
      default:
         break;
      }
      if (rep)
      {
         size_t rlen = strlen(rep);
         if (used + rlen >= dst_len)
            break;
         memcpy(dst + used, rep, rlen);
         used += rlen;
      }
      else
         dst[used++] = (char)*p;
   }
   dst[used] = '\0';
}

/* Map a context section header to the XML tag wrapping its items. Unknown or
 * NULL headers fall back to the generic "memory_item". */
static inline const char *context_xml_tag_for_header(const char *header)
{
   if (!header)
      return "memory_item";
   if (strcmp(header, "Key Facts") == 0)
      return "historical_fact";
   if (strcmp(header, "Mental Models") == 0)
      return "mental_model";
   if (strcmp(header, "Constraints") == 0)
      return "constraint";
   if (strcmp(header, "Procedures") == 0)
      return "procedure_memory";
   if (strcmp(header, "Active Tasks") == 0)
      return "active_task";
   if (strcmp(header, "Recent Context") == 0)
      return "recent_event";
   return "memory_item";
}

#endif /* DEC_MEMORY_ASSEMBLE_UTIL_H */
