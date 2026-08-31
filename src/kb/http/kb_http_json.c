/* kb_http_json.c: scalar field scanners for KB HTTP request bodies.
 * See kb_http_json.h for what these are and are not. */

#include "kb_http_json.h"
#include "aimee.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kb_http_capabilities_json(char *out, size_t out_cap, cJSON *surfaces)
{
   cJSON *root = cJSON_CreateObject();
   cJSON *capabilities = cJSON_AddArrayToObject(root, "capabilities");
   cJSON_AddItemToArray(capabilities, cJSON_CreateString("memory"));
   cJSON_AddItemToArray(capabilities, cJSON_CreateString("search"));
   cJSON_AddItemToArray(capabilities, cJSON_CreateString("index"));
   cJSON_AddItemToObject(root, "agent_surfaces", surfaces ? surfaces : cJSON_CreateObject());
   cJSON_AddStringToObject(root, "version", AIMEE_VERSION);
   char *json = cJSON_PrintUnformatted(root);
   snprintf(out, out_cap, "%s", json ? json : "{\"error\":\"out of memory\"}");
   free(json);
   cJSON_Delete(root);
}

/* Position p just past `"key" :` and return it, or NULL when the key is absent
 * or is not followed by a colon. Shared by all three scanners so they agree on
 * what "the value of this key" means. */
static const char *json_value_at(const char *body, const char *key)
{
   if (!body)
      return NULL;
   char needle[128];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(body, needle);
   if (!p)
      return NULL;
   p += strlen(needle);
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != ':')
      return NULL;
   p++;
   while (*p == ' ' || *p == '\t')
      p++;
   return p;
}

int kb_http_json_str(const char *body, const char *key, char *out, size_t out_cap)
{
   if (!body || !out_cap)
      return 0;
   out[0] = '\0';
   const char *p = json_value_at(body, key);
   if (!p || *p != '"')
      return 0;
   p++;
   size_t i = 0;
   while (*p && *p != '"' && i + 1 < out_cap)
   {
      if (*p == '\\' && *(p + 1))
         p++;
      out[i++] = *p++;
   }
   out[i] = '\0';
   return 1;
}

int kb_http_json_int(const char *body, const char *key, int default_val)
{
   const char *p = json_value_at(body, key);
   if (!p)
      return default_val;

   /* A leading sign is part of the number. Testing p[0] against '0'..'9'
    * rejected every negative, so the caller silently received its default and
    * could not tell "-1 was sent" from "nothing was sent". */
   const char *digits = p;
   if (*digits == '-' || *digits == '+')
      digits++;
   if (*digits < '0' || *digits > '9')
      return default_val;

   /* strtol rather than atoi: atoi on an out-of-range literal is undefined,
    * and this parses untrusted request bodies. Saturate instead. */
   errno = 0;
   long v = strtol(p, NULL, 10);
   if (errno == ERANGE)
      return (v < 0) ? INT_MIN : INT_MAX;
   if (v > INT_MAX)
      return INT_MAX;
   if (v < INT_MIN)
      return INT_MIN;
   return (int)v;
}

int kb_http_json_bool(const char *body, const char *key, int default_val)
{
   const char *p = json_value_at(body, key);
   if (!p)
      return default_val;
   if (strncmp(p, "true", 4) == 0)
      return 1;
   if (strncmp(p, "false", 5) == 0)
      return 0;
   return default_val;
}
