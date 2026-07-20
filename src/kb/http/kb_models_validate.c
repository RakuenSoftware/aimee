/* kb_models_validate.c: pure input validators for the P2a /v1/models routes.
 * See kb_models_validate.h. No storage/transport deps — unit-testable in isolation. */
#include "kb_models_validate.h"

#include <string.h>

int kb_models_wire_valid(const char *wire)
{
   if (!wire)
      return 0;
   return strcmp(wire, "anthropic") == 0 || strcmp(wire, "openai") == 0 ||
          strcmp(wire, "responses") == 0 || strcmp(wire, "gemini") == 0;
}

int kb_models_name_clean(const char *s, int max)
{
   if (!s)
      return 0;
   size_t n = strlen(s);
   if (n == 0 || (int)n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f)
         return 0;
   return 1;
}
