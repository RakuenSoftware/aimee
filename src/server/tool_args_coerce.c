#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "tool_args_coerce.h"
#include "cJSON.h"

/* Returns 1 if prop_schema declares `type` == want (string or array form). */
static int schema_declares_type(const cJSON *prop_schema, const char *want)
{
   if (!prop_schema)
      return 0;

   const cJSON *type = cJSON_GetObjectItemCaseSensitive(prop_schema, "type");
   if (!type)
      return 0;

   if (cJSON_IsString(type) && strcmp(type->valuestring, want) == 0)
      return 1;

   if (cJSON_IsArray(type))
   {
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, type)
      {
         if (cJSON_IsString(item) && strcmp(item->valuestring, want) == 0)
            return 1;
      }
   }

   return 0;
}

/* Try to coerce a string scalar to the given primitive type.
 * Returns a new cJSON on success, NULL on failure. */
static cJSON *coerce_string_to(const cJSON *src, const char *target_type)
{
   if (!cJSON_IsString(src))
      return NULL;

   const char *s = src->valuestring;

   if (strcmp(target_type, "integer") == 0)
   {
      char *end = NULL;
      long val = strtol(s, &end, 10);
      while (*end && isspace((unsigned char)*end))
         end++;
      if (*end != '\0')
         return NULL;
      return cJSON_CreateNumber(val);
   }

   if (strcmp(target_type, "number") == 0)
   {
      char *end = NULL;
      double val = strtod(s, &end);
      while (*end && isspace((unsigned char)*end))
         end++;
      if (*end != '\0')
         return NULL;
      return cJSON_CreateNumber(val);
   }

   if (strcmp(target_type, "boolean") == 0)
   {
      if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0)
         return cJSON_CreateTrue();
      if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0)
         return cJSON_CreateFalse();
      return NULL;
   }

   return NULL;
}

/* Coerce one property value given its per-property schema.
 * Returns a new cJSON (caller owns) or NULL to indicate "use original". */
static cJSON *coerce_one(const cJSON *val, const cJSON *prop_schema)
{
   if (!prop_schema)
      return NULL;

   /* integer: string "42" → 42 */
   if (schema_declares_type(prop_schema, "integer") && cJSON_IsString(val))
   {
      cJSON *res = coerce_string_to(val, "integer");
      if (res)
         return res;
   }

   /* number: string "3.14" → 3.14 */
   if (schema_declares_type(prop_schema, "number") && cJSON_IsString(val))
   {
      cJSON *res = coerce_string_to(val, "number");
      if (res)
         return res;
   }

   /* boolean: string "true"/"1" → true, "false"/"0" → false */
   if (schema_declares_type(prop_schema, "boolean") && cJSON_IsString(val))
   {
      cJSON *res = coerce_string_to(val, "boolean");
      if (res)
         return res;
   }

   /* array: scalar → [scalar], with optional items.type coercion */
   if (schema_declares_type(prop_schema, "array") && !cJSON_IsArray(val))
   {
      cJSON *arr = cJSON_CreateArray();
      cJSON *elem = NULL;

      const cJSON *items = cJSON_GetObjectItemCaseSensitive(prop_schema, "items");
      if (items)
      {
         const cJSON *items_type = cJSON_GetObjectItemCaseSensitive(items, "type");
         if (items_type && cJSON_IsString(items_type))
         {
            elem = coerce_string_to(val, items_type->valuestring);
         }
      }

      if (elem)
         cJSON_AddItemToArray(arr, elem);
      else
         cJSON_AddItemToArray(arr, cJSON_Duplicate(val, 1));

      return arr;
   }

   /* object: JSON string → parsed object */
   if (schema_declares_type(prop_schema, "object") && cJSON_IsString(val))
   {
      const char *p = val->valuestring;
      while (*p && isspace((unsigned char)*p))
         p++;
      if (*p == '{')
      {
         cJSON *parsed = cJSON_Parse(p);
         if (parsed)
            return parsed;
      }
   }

   return NULL;
}

cJSON *tool_args_coerce(const cJSON *declared_schema, const cJSON *raw_args)
{
   if (!declared_schema)
      return cJSON_Duplicate(raw_args, 1);

   if (!cJSON_IsObject(raw_args))
      return cJSON_Duplicate(raw_args, 1);

   if (!schema_declares_type(declared_schema, "object"))
      return cJSON_Duplicate(raw_args, 1);

   const cJSON *properties = cJSON_GetObjectItemCaseSensitive(declared_schema, "properties");
   if (!properties || !cJSON_IsObject(properties))
      return cJSON_Duplicate(raw_args, 1);

   cJSON *result = cJSON_CreateObject();
   if (!result)
      return NULL;

   cJSON *child = NULL;
   cJSON_ArrayForEach(child, raw_args)
   {
      const cJSON *prop_schema = cJSON_GetObjectItemCaseSensitive(properties, child->string);
      cJSON *coerced = coerce_one(child, prop_schema);

      /* Always add an owning copy. Reference items would leave the result
       * holding dangling pointers as soon as the caller cJSON_Deletes the
       * input — the dispatch path does exactly that to swap args. */
      if (coerced)
         cJSON_AddItemToObject(result, child->string, coerced);
      else
         cJSON_AddItemToObject(result, child->string, cJSON_Duplicate(child, 1));
   }

   return result;
}

/* --- Canonicalization: one shape, decided once ----------------------------
 *
 * WHY THIS EXISTS
 *
 * A tool call used to be normalized twice, differently, and authorized on
 * neither result. agent_policy.c's validator resolved aliases and coerced ints
 * on a copy it then threw away; the dispatcher resolved a DIFFERENT alias set
 * and ran the schema coercion below, and that result is what executed. In
 * between, execution-policy and the audit record both saw the raw arguments.
 *
 * So the authorizer could permit {"paths": "/etc/passwd"} while the tool ran
 * {"paths": ["/etc/passwd"]}, and the ledger recorded arguments that never
 * executed. The fix is not a better transform, it is a single one: the caller
 * canonicalizes once, before the gates, and every gate plus the executor plus
 * the audit record see the same bytes.
 *
 * Idempotent by construction, so the dispatcher can keep calling it for its
 * other entry points (script RPC, the MCP gateway) without double-transforming
 * a turn the agent loop already canonicalized. */

/* Names a model commonly emits instead of a tool's declared parameter. The
 * union of the two tables that previously disagreed. */
typedef struct
{
   const char *alias;
   const char *canonical;
} arg_alias_t;

static const arg_alias_t g_arg_aliases[] = {
    {"filepath", "path"},  {"file_path", "path"},  {"file", "path"},      {"filename", "path"},
    {"file_name", "path"}, {"dir", "path"},        {"directory", "path"}, {"cmd", "command"},
    {"q", "query"},        {"max", "max_results"}, {"cnt", "count"},      {"num", "count"},
    {"msg", "message"},    {NULL, NULL},
};

/* Fields historically coerced by name for tools with no registry schema. */
static const char *g_int_fields[] = {"offset", "limit", "count", "max_results", NULL};

static void apply_aliases(cJSON *args, const cJSON *properties)
{
   for (const arg_alias_t *a = g_arg_aliases; a->alias; a++)
   {
      if (!cJSON_GetObjectItem(args, a->alias))
         continue;
      if (cJSON_GetObjectItem(args, a->canonical))
         continue; /* the real parameter is already present */
      if (properties)
      {
         /* With a schema in hand, rewrite ONLY when the alias is not itself a
          * declared parameter and the canonical name is one. A tool with a real
          * `file` parameter and no `path` keeps its argument; the unguarded
          * rewrite the dispatcher used to do would have silently renamed it. */
         if (cJSON_GetObjectItemCaseSensitive(properties, a->alias))
            continue;
         if (!cJSON_GetObjectItemCaseSensitive(properties, a->canonical))
            continue;
      }
      cJSON *detached = cJSON_DetachItemFromObject(args, a->alias);
      if (detached)
         cJSON_AddItemToObject(args, a->canonical, detached);
   }
}

static void coerce_named_int_fields(cJSON *args)
{
   for (int i = 0; g_int_fields[i]; i++)
   {
      cJSON *f = cJSON_GetObjectItem(args, g_int_fields[i]);
      if (!f || !cJSON_IsString(f))
         continue;
      char *end = NULL;
      long v = strtol(f->valuestring, &end, 10);
      if (end && *end == '\0' && f->valuestring != end)
         cJSON_ReplaceItemInObject(args, g_int_fields[i], cJSON_CreateNumber((double)v));
   }
}

cJSON *tool_args_canonicalize(const cJSON *declared_schema, const cJSON *raw_args)
{
   if (!cJSON_IsObject(raw_args))
      return cJSON_Duplicate(raw_args, 1);

   cJSON *work = cJSON_Duplicate(raw_args, 1);
   if (!work)
      return NULL;

   const cJSON *properties =
       declared_schema ? cJSON_GetObjectItemCaseSensitive(declared_schema, "properties") : NULL;
   if (properties && !cJSON_IsObject(properties))
      properties = NULL;

   apply_aliases(work, properties);
   coerce_named_int_fields(work);

   cJSON *coerced = tool_args_coerce(declared_schema, work);
   cJSON_Delete(work);
   return coerced;
}

char *tool_args_canonicalize_json(const cJSON *declared_schema, const char *raw_args_json)
{
   if (!raw_args_json)
      return NULL;
   cJSON *raw = cJSON_Parse(raw_args_json);
   if (!raw)
      return NULL; /* invalid JSON is the validator's error to report, not ours */
   cJSON *canonical = tool_args_canonicalize(declared_schema, raw);
   cJSON_Delete(raw);
   if (!canonical)
      return NULL;
   char *out = cJSON_PrintUnformatted(canonical);
   cJSON_Delete(canonical);
   return out;
}
