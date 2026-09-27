#ifndef AIMEE_GIT_COMMAND_H
#define AIMEE_GIT_COMMAND_H
#include <ctype.h>
#include <string.h>

/* Aimee Git owns target resolution and mutation policy. Native hooks must not
 * reinterpret its arguments as a raw git invocation in the session checkout.
 * Reject compound commands, redirections and expansions; this is deliberately
 * a narrow recognizer, not a shell evaluator. */
static inline int aimee_git_command(const char *command)
{
   if (!command)
      return 0;
   int count = 0;
   const char *p = command;
   while (*p)
   {
      while (*p == ' ' || *p == '\t')
         p++;
      if (!*p)
         break;
      char word[4096];
      size_t n = 0;
      char quote = 0;
      while (*p && (quote || (*p != ' ' && *p != '\t')))
      {
         char c = *p++;
         if (c == '\n' || c == '\r' || c == '$' || c == '`' || c == '\\')
            return 0;
         if (!quote && (c == '\'' || c == '"'))
         {
            quote = c;
            continue;
         }
         if (quote && c == quote)
         {
            quote = 0;
            continue;
         }
         if (!quote && strchr(";&|<>()*?[]{}", c))
            return 0;
         if (n + 1 >= sizeof(word))
            return 0;
         word[n++] = c;
      }
      if (quote)
         return 0;
      word[n] = '\0';
      if (count == 0)
      {
         const char *base = strrchr(word, '/');
         if (strcmp(base ? base + 1 : word, "aimee") != 0)
            return 0;
      }
      if (count == 1 && strcmp(word, "git") != 0)
         return 0;
      count++;
   }
   return count >= 3;
}
#endif
