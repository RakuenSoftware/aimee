/* fact_grounding.c: see fact_grounding.h. */
#include "fact_grounding.h"

#include <ctype.h>
#include <string.h>

/* Lowercase, punctuation to spaces, runs collapsed. Underscores and hyphens go
 * too, so a model writing kb_server matches a note saying "KB server". */
void fact_norm_text(const char *in, char *out, size_t cap)
{
   size_t o = 0;
   int sp = 1; /* leading-space suppression */
   for (const char *p = in ? in : ""; *p && o + 1 < cap; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c) || c == '.' || c == ':')
      {
         out[o++] = (char)tolower(c);
         sp = 0;
      }
      else if (!sp)
      {
         out[o++] = ' ';
         sp = 1;
      }
   }
   while (o > 0 && out[o - 1] == ' ')
      o--;
   out[o] = '\0';
}

/* Is `value` traceable to the note? Whole-string match, else a majority of its
 * content words must appear. "user" is grounded by convention: the prompt tells
 * the model to use it as the subject of a first-person note. */
int fact_grounded(const char *value, const char *note_norm)
{
   char v[512];
   fact_norm_text(value, v, sizeof(v));
   if (!v[0] || strcmp(v, "user") == 0 || strcmp(v, "i") == 0 || strcmp(v, "me") == 0)
      return 1;
   if (strstr(note_norm, v))
      return 1;

   int words = 0, hits = 0;
   for (char *tok = strtok(v, " "); tok; tok = strtok(NULL, " "))
   {
      if (strlen(tok) <= 2) /* skip articles and short filler */
         continue;
      words++;
      if (strstr(note_norm, tok))
         hits++;
   }
   if (!words)
      return 0;
   return hits * 2 >= words;
}
