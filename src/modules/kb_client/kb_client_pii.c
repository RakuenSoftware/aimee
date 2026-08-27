/* kb_client_pii.c: the client-side screen that keeps secrets and PII from
 * crossing aimee-server -> aimee-kb. See kb_client_pii.h for the contract and
 * for why the screen lives here rather than at each call site. */
#include "kb_client_pii.h"

#include <stdlib.h>
#include <string.h>

/* gate_check_sensitive is the platform secret/PII classifier, implemented in
 * posix/memory.c and windows/memory.c -- platform files, not inside the memory
 * module. Its only header lives at modules/memory/memory_platform.h, which is
 * module-internal and deliberately not on this translation unit's include path:
 * the memory module publishes a bus wire contract, not a C surface. Declaring
 * the prototype here keeps the boundary intact while still calling the one
 * canonical classifier rather than growing a second, divergent one. */
int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap);

int kb_client_pii_screen(const char *text, char **out)
{
   *out = NULL;
   if (!text || !text[0])
      return 0;
   /* Sized so the prefix plus the [REDACTED] marker always fits; the classifier
    * only reports 2 for a span it could not locate, never for a small buffer. */
   size_t cap = strlen(text) + 32;
   char *buf = malloc(cap);
   if (!buf)
      return -1;
   int verdict = gate_check_sensitive(text, buf, cap);
   if (verdict == 1)
   {
      *out = buf;
      return 0;
   }
   free(buf);
   return verdict == 0 ? 0 : -1;
}

int kb_client_pii_identifier_sensitive(const char *ident)
{
   if (!ident || !ident[0])
      return 0;
   size_t cap = strlen(ident) + 32;
   char *buf = malloc(cap);
   if (!buf)
      return 1;
   int verdict = gate_check_sensitive(ident, buf, cap);
   free(buf);
   return verdict != 0;
}

static int kbc_pii_add(cJSON *obj, const char *field, const char *text, int required)
{
   if (!obj || !field)
      return -1;
   if (!text || !text[0])
   {
      if (required && text)
         cJSON_AddStringToObject(obj, field, "");
      return 0;
   }
   char *red = NULL;
   if (kb_client_pii_screen(text, &red) != 0)
      return -1;
   cJSON_AddStringToObject(obj, field, red ? red : text);
   free(red);
   return 0;
}

int kb_client_pii_add_string(cJSON *obj, const char *field, const char *text)
{
   return kbc_pii_add(obj, field, text, 0);
}

int kb_client_pii_add_string_required(cJSON *obj, const char *field, const char *text)
{
   return kbc_pii_add(obj, field, text, 1);
}

char *kb_client_pii_withheld_json(void)
{
   return strdup("{\"status\":\"error\",\"message\":\"withheld_pii: content was not sent to "
                 "aimee-kb\"}");
}
