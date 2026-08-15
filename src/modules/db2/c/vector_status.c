#include "vector_status.h"
#include "pgvec_transport.h"

cJSON *pgvec_vector_status_json(void)
{
   int mem_ready = pgvec_table_ready(PGVEC_MEMORY_TABLE);
   int kb_ready = pgvec_table_ready(PGVEC_KB_TABLE);
   int available = (mem_ready >= 0 && kb_ready >= 0);

   cJSON *vector = cJSON_CreateObject();
   if (!vector)
      return NULL;

   cJSON_AddStringToObject(vector, "owner", "knowledge-service");
   cJSON_AddBoolToObject(vector, "available", available ? 1 : 0);
   cJSON_AddStringToObject(vector, "backend", "pgvector");
   cJSON_AddStringToObject(vector, "memory_collection", PGVEC_MEMORY_TABLE);
   cJSON_AddStringToObject(vector, "kb_collection", PGVEC_KB_TABLE);
   cJSON_AddBoolToObject(vector, "memory_collection_ready", mem_ready > 0);
   cJSON_AddBoolToObject(vector, "kb_collection_ready", kb_ready > 0);

   if (mem_ready > 0)
      cJSON_AddNumberToObject(vector, "memory_points",
                              (double)pgvec_point_count(PGVEC_MEMORY_TABLE));
   if (kb_ready > 0)
      cJSON_AddNumberToObject(vector, "kb_points", (double)pgvec_point_count(PGVEC_KB_TABLE));

   if (!available)
   {
      cJSON_AddStringToObject(vector, "status", "unavailable");
      cJSON_AddStringToObject(vector, "message", "pgvector tables not accessible");
   }
   else if (mem_ready <= 0 || kb_ready <= 0)
   {
      cJSON_AddStringToObject(vector, "status", "degraded");
      cJSON_AddStringToObject(vector, "message",
                              "pgvector tables exist but HNSW index missing on one or more");
   }
   else
   {
      cJSON_AddStringToObject(vector, "status", "ok");
      cJSON_AddStringToObject(vector, "message", "pgvector ready");
   }

   return vector;
}
