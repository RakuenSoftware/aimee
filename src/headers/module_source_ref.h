/* Generic opaque module references. Schemes and versions belong to the owner;
 * the host compares matching domains and never interprets database coordinates.
 * A lookup reference is not evidence that earlier text was served from it. */
#ifndef AIMEE_MODULE_SOURCE_REF_H
#define AIMEE_MODULE_SOURCE_REF_H

#include "cJSON.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum
{
   MODULE_SOURCE_REF_UNKNOWN = -1,
   MODULE_SOURCE_REF_EQUAL = 0,
   MODULE_SOURCE_REF_CHANGED = 1
} module_source_ref_comparison_t;

static inline int module_source_ref_text(const cJSON *value, size_t maximum)
{
   const char *text = cJSON_GetStringValue(value);
   if (!text || !text[0])
      return 0;
   size_t length = 0;
   for (; text[length]; ++length)
      if (length >= maximum || (unsigned char)text[length] < 32 ||
          (unsigned char)text[length] > 126)
         return 0;
   return 1;
}

static inline int module_source_ref_valid(const cJSON *reference, int64_t expected_id)
{
   if (!cJSON_IsObject(reference) || expected_id <= 0 || cJSON_GetArraySize(reference) != 5)
      return 0;
   const cJSON *field;
   cJSON_ArrayForEach(field,
                      reference) if (!field->string || (strcmp(field->string, "schema_version") &&
                                                        strcmp(field->string, "scheme") &&
                                                        strcmp(field->string, "owner") &&
                                                        strcmp(field->string, "record") &&
                                                        strcmp(field->string, "version"))) return 0;
   const cJSON *schema = cJSON_GetObjectItemCaseSensitive(reference, "schema_version");
   if (!cJSON_IsNumber(schema) || schema->valuedouble != 1.0 ||
       !module_source_ref_text(cJSON_GetObjectItemCaseSensitive(reference, "scheme"), 128) ||
       !module_source_ref_text(cJSON_GetObjectItemCaseSensitive(reference, "owner"), 512) ||
       !module_source_ref_text(cJSON_GetObjectItemCaseSensitive(reference, "version"), 4096))
      return 0;
   char id[32];
   snprintf(id, sizeof(id), "%" PRId64, expected_id);
   const char *record = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reference, "record"));
   return record && strcmp(record, id) == 0;
}

/* 1 copied, 0 absent, -1 malformed or allocation failure. */
static inline int module_source_ref_copy(cJSON *destination, const cJSON *source, const char *field,
                                         int64_t expected_id)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(source, field);
   if (!value)
      return 0;
   if (!module_source_ref_valid(value, expected_id))
      return -1;
   cJSON *copy = cJSON_Duplicate(value, 1);
   if (!copy || !cJSON_AddItemToObject(destination, field, copy))
   {
      cJSON_Delete(copy);
      return -1;
   }
   return 1;
}

static inline module_source_ref_comparison_t
module_source_ref_compare(const cJSON *captured, const cJSON *current, int64_t expected_id)
{
   if (!module_source_ref_valid(captured, expected_id) ||
       !module_source_ref_valid(current, expected_id))
      return MODULE_SOURCE_REF_UNKNOWN;
   const char *domain[] = {"scheme", "owner", "record"};
   for (size_t i = 0; i < sizeof(domain) / sizeof(domain[0]); ++i)
      if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(captured, domain[i])),
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, domain[i]))))
         return MODULE_SOURCE_REF_UNKNOWN;
   return strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(captured, "version")),
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(current, "version")))
              ? MODULE_SOURCE_REF_CHANGED
              : MODULE_SOURCE_REF_EQUAL;
}

/* Legacy strings are comparable only when neither side supplies a new-domain
 * reference. Missing or mixed-domain evidence remains unknown. */
static inline module_source_ref_comparison_t
module_source_ref_compare_legacy(const cJSON *captured, const cJSON *current, int64_t expected_id,
                                 const char *captured_legacy, const char *current_legacy)
{
   if (captured || current)
      return module_source_ref_compare(captured, current, expected_id);
   if (!captured_legacy || !captured_legacy[0] || !current_legacy || !current_legacy[0])
      return MODULE_SOURCE_REF_UNKNOWN;
   return strcmp(captured_legacy, current_legacy) ? MODULE_SOURCE_REF_CHANGED
                                                  : MODULE_SOURCE_REF_EQUAL;
}

#endif
