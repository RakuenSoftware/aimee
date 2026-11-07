/* Decode the memory module's embedding messages without losing their shape or
 * decline/truncation facts. This is a client codec, not an embedding engine. */
#ifndef AIMEE_EMBED_RESULT_H
#define AIMEE_EMBED_RESULT_H

#include "json_fluent.h"
#include <string.h>

static inline int embed_reply_dimension(const cJSON *reply, int capacity)
{
   if (!cJSON_IsObject(reply) || capacity <= 0)
      return 0;
   const char *flags[] = {"unavailable", "unauthorized", "truncated"};
   for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
   {
      const cJSON *flag = cJSON_GetObjectItemCaseSensitive(reply, flags[i]);
      if (flag && (!cJSON_IsBool(flag) || cJSON_IsTrue(flag)))
         return 0;
   }
   const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
   if (error && (!cJSON_IsString(error) || !error->valuestring || error->valuestring[0]))
      return 0;
   const cJSON *dim = cJSON_GetObjectItemCaseSensitive(reply, "dim");
   if (!cJSON_IsNumber(dim) || !isfinite(dim->valuedouble) || dim->valuedouble <= 0 ||
       dim->valuedouble > capacity || floor(dim->valuedouble) != dim->valuedouble)
      return 0;
   return (int)dim->valuedouble;
}

/* Returns the complete dimension; failure leaves the caller's buffer untouched. */
static inline int embed_reply_vector(const cJSON *reply, float *out, int capacity)
{
   int dim = embed_reply_dimension(reply, capacity);
   const cJSON *vector = cJSON_GetObjectItemCaseSensitive(reply, "vector");
   if (!dim || cJSON_GetArraySize(vector) != dim)
      return 0;
   return jo_float_array(vector, out, capacity);
}

/* Rows are placed at the caller's stride, while the return value is vector
 * width, not row count. Validate the complete matrix before publishing any row. */
static inline int embed_reply_matrix(const cJSON *reply, float *out, int rows, int stride)
{
   int dim = embed_reply_dimension(reply, stride);
   if (!dim || !out || rows <= 0 || (size_t)rows > SIZE_MAX / sizeof(float) / (size_t)stride)
      return 0;
   const cJSON *vectors = cJSON_GetObjectItemCaseSensitive(reply, "vectors");
   if (jo_float_matrix(vectors, out, rows, dim) != rows)
      return 0;
   /* jo_float_matrix writes a compact matrix. Expand backwards so a narrower
    * provider retains every component at the caller's fixed-capacity stride. */
   for (int i = rows - 1; i > 0; --i)
      memmove(out + (size_t)i * stride, out + (size_t)i * dim, (size_t)dim * sizeof(float));
   return dim;
}

#endif
