#include "skill_trigger_policy.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static int value_contains_token(const char *value, size_t value_len, const char *needle)
{
   if (!value || !needle || !needle[0])
      return 0;
   size_t needle_len = strlen(needle);
   const char *p = value;
   const char *end = value + value_len;
   while (p < end)
   {
      while (p < end && !(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.'))
         p++;
      const char *start = p;
      while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.'))
         p++;
      if ((size_t)(p - start) == needle_len && strncmp(start, needle, needle_len) == 0)
         return 1;
   }
   return 0;
}

static int patterns_match(const char *value, size_t value_len, const char *subject)
{
   if (!value || !subject)
      return 0;

   const char *p = value;
   const char *end = value + value_len;
   int saw_quoted = 0;
   while (p < end)
   {
      while (p < end && *p != '"' && *p != '\'')
         p++;
      if (p >= end)
         break;
      char quote = *p++;
      const char *start = p;
      while (p < end && *p != quote)
         p++;
      size_t pat_len = (size_t)(p - start);
      if (pat_len > 0)
      {
         saw_quoted = 1;
         char pattern[128];
         if (pat_len >= sizeof(pattern))
            pat_len = sizeof(pattern) - 1;
         memcpy(pattern, start, pat_len);
         pattern[pat_len] = '\0';
         if (strstr(subject, pattern))
            return 1;
      }
      if (p < end)
         p++;
   }
   return saw_quoted ? 0 : value_contains_token(value, value_len, subject);
}

static int line_value(const char *line, size_t line_len, const char *key, const char **value,
                      size_t *value_len)
{
   size_t key_len = strlen(key);
   const char *p = line;
   const char *end = line + line_len;
   while (p < end && isspace((unsigned char)*p))
      p++;
   if ((size_t)(end - p) < key_len + 1 || strncmp(p, key, key_len) != 0 || p[key_len] != ':')
      return 0;
   p += key_len + 1;
   while (p < end && isspace((unsigned char)*p))
      p++;
   while (end > p && isspace((unsigned char)end[-1]))
      end--;
   *value = p;
   *value_len = (size_t)(end - p);
   return 1;
}

int skill_trigger_policy_matches_content(const char *content, const char *tool_name,
                                         const char *subject)
{
   if (!content || !tool_name || !tool_name[0])
      return 0;
   if (!subject)
      subject = "";
   if (strncmp(content, "---", 3) != 0 || (content[3] != '\n' && content[3] != '\r'))
      return 0;

   const char *header_start = strchr(content, '\n');
   const char *header_end = header_start ? strstr(header_start + 1, "\n---") : NULL;
   if (!header_start || !header_end)
      return 0;

   int in_triggers = 0;
   int saw_triggers = 0;
   int saw_tool = 0, tool_match = 0;
   int saw_pattern = 0, pattern_match = 0;
   const char *p = header_start + 1;
   while (p < header_end)
   {
      const char *line_end = memchr(p, '\n', (size_t)(header_end - p));
      if (!line_end)
         line_end = header_end;
      size_t line_len = (size_t)(line_end - p);
      const char *value = NULL;
      size_t value_len = 0;

      if (line_len == 0)
      {
         p = line_end < header_end ? line_end + 1 : header_end;
         continue;
      }
      if (!isspace((unsigned char)p[0]))
      {
         if (strncmp(p, "triggers:", 9) == 0)
         {
            in_triggers = 1;
            saw_triggers = 1;
         }
         else if (in_triggers)
            break;
      }
      else if (in_triggers)
      {
         if (line_value(p, line_len, "tool", &value, &value_len))
         {
            saw_tool = 1;
            if (value_contains_token(value, value_len, tool_name))
               tool_match = 1;
         }
         else if (line_value(p, line_len, "arg_pattern", &value, &value_len) ||
                  line_value(p, line_len, "path_pattern", &value, &value_len))
         {
            saw_pattern = 1;
            if (patterns_match(value, value_len, subject))
               pattern_match = 1;
         }
      }
      p = line_end < header_end ? line_end + 1 : header_end;
   }

   return saw_triggers && (!saw_tool || tool_match) && (!saw_pattern || pattern_match);
}
