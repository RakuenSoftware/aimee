/* kb_client_pii.c: the client-side screen that keeps secrets and PII from
 * crossing aimee-server -> aimee-kb. See kb_client_pii.h for the contract and
 * for why the screen lives here rather than at each call site. */
#include "kb_client_pii.h"
#include "module_commands.h"

#include <stdlib.h>
#include <string.h>

int kb_client_pii_screen(const char *text, char **out)
{
   *out = NULL;
   if (!text || !text[0])
      return 0;
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request || !cJSON_AddStringToObject(request, "content", text))
   {
      cJSON_Delete(request);
      return -1;
   }
   int dispatched = aimee_module_commands_dispatch("memory.screen_content", request, &response);
   cJSON_Delete(request);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   const char *verdict =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "verdict"));
   int rc = -1;
   if (dispatched == 1 && status && strcmp(status, "ok") == 0 && verdict)
   {
      if (strcmp(verdict, "allow") == 0)
         rc = 0;
      else if (strcmp(verdict, "redact") == 0)
      {
         const char *redacted =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "redacted"));
         if (redacted && (*out = strdup(redacted)))
            rc = 0;
      }
   }
   cJSON_Delete(response);
   return rc;
}

int kb_client_pii_identifier_sensitive(const char *ident)
{
   char *redacted = NULL;
   int sensitive = kb_client_pii_screen(ident, &redacted) != 0 || redacted != NULL;
   free(redacted);
   return sensitive;
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
