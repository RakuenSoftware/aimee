/* fold_register.c: register/trust grammar for assistant turns (fold §6, P3).
 * See fold_register.h. Deterministic; ASCII bracket tags + UTF-8 glyph prefixes. */
#include "fold_register.h"

#include <string.h>

/* ASCII lower without locale dependence (the bracket tags are ASCII-only). */
static int a_lower(int c)
{
   return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Match a bracketed tag: `p` points just past '['. Returns 1 iff `tag` matches
 * (case-insensitive) AND is immediately followed by the closing ']', so only
 * exact tags match ("[exec]" yes; "[executable]" no). NUL-safe: a NUL in `p`
 * mismatches the non-NUL tag char and returns 0 without reading past it. */
static int btag(const char *p, const char *tag)
{
   size_t i = 0;
   for (; tag[i]; i++)
      if (a_lower((unsigned char)p[i]) != a_lower((unsigned char)tag[i]))
         return 0;
   return p[i] == ']'; /* require the closing bracket right after the tag */
}

fold_register_t fold_register_parse(const char *text)
{
   if (!text)
      return FOLD_REG_IN_PROGRESS;
   const char *p = text;
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;

   /* leading glyphs (UTF-8 byte sequences). strncmp (not memcmp) so a short input
    * like "\xF0" alone stops at the NUL mismatch instead of reading past it. */
   if ((unsigned char)p[0] == 0xF0) /* 4-byte glyph */
   {
      if (strncmp(p, "\xF0\x9F\x8F\x81", 4) == 0) /* 🏁 */
         return FOLD_REG_VERDICT;
      if (strncmp(p, "\xF0\x9F\x94\x8D", 4) == 0) /* 🔍 */
         return FOLD_REG_IN_PROGRESS;
   }
   if ((unsigned char)p[0] == 0xE2) /* 3-byte glyph */
   {
      if (strncmp(p, "\xE2\x96\xB6", 3) == 0) /* ▶ */
         return FOLD_REG_EXECUTING;
      if (strncmp(p, "\xE2\x9A\xA0", 3) == 0) /* ⚠ */
         return FOLD_REG_HAZARD;
      if (strncmp(p, "\xE2\x9D\x93", 3) == 0) /* ❓ */
         return FOLD_REG_BLOCKED;
   }

   /* bracketed word tags (exact, closing-bracket-anchored) */
   if (*p == '[')
   {
      const char *t = p + 1;
      if (btag(t, "verdict") || btag(t, "done"))
         return FOLD_REG_VERDICT;
      if (btag(t, "hazard") || btag(t, "warning"))
         return FOLD_REG_HAZARD;
      if (btag(t, "executing") || btag(t, "exec"))
         return FOLD_REG_EXECUTING;
      if (btag(t, "blocked"))
         return FOLD_REG_BLOCKED;
      if (btag(t, "in-progress") || btag(t, "wip"))
         return FOLD_REG_IN_PROGRESS;
   }
   return FOLD_REG_IN_PROGRESS;
}

const char *fold_register_label(fold_register_t r)
{
   switch (r)
   {
   case FOLD_REG_EXECUTING:
      return "exec";
   case FOLD_REG_VERDICT:
      return "verdict";
   case FOLD_REG_HAZARD:
      return "hazard";
   case FOLD_REG_BLOCKED:
      return "blocked";
   default:
      return "wip";
   }
}

int fold_register_is_settled(fold_register_t r)
{
   return r == FOLD_REG_VERDICT || r == FOLD_REG_HAZARD;
}
