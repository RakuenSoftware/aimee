/* Descriptor-owned DB2 process support for model-catalog admission policy. */
#include "db2_model_validation.h"

#include <string.h>

int kb_models_wire_valid(const char *wire)
{
   if (!wire)
      return 0;
   return strcmp(wire, "anthropic") == 0 || strcmp(wire, "openai") == 0 ||
          strcmp(wire, "responses") == 0 || strcmp(wire, "gemini") == 0;
}

int kb_models_name_clean(const char *value, int max)
{
   if (!value)
      return 0;
   size_t length = strlen(value);
   if (length == 0 || (int)length > max)
      return 0;
   for (size_t i = 0; i < length; ++i)
      if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] == 0x7f)
         return 0;
   return 1;
}

int kb_models_endpoint_valid(const char *endpoint, int max)
{
   if (!endpoint)
      return 0;
   size_t length = strlen(endpoint);
   if (length == 0)
      return 1;
   if ((int)length > max)
      return 0;
   for (size_t i = 0; i < length; ++i)
      if ((unsigned char)endpoint[i] < 0x20 || (unsigned char)endpoint[i] == 0x7f)
         return 0;
   if (strncmp(endpoint, "https://", 8) != 0 && strncmp(endpoint, "http://", 7) != 0)
      return 0;
   return 1;
}
