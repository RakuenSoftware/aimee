#include "vector_status.h"
#include "pgvec_transport.h"

/* The KB's own vector collection. Memory vectors belong to the memory
 * implementation and report through its health. */
cJSON *pgvec_vector_status_json(void)
{
   int kb_ready = pgvec_table_ready(PGVEC_KB_TABLE);
   int available = kb_ready >= 0;

   cJSON *vector = cJSON_CreateObject();
   if (!vector)
      return NULL;

   cJSON_AddStringToObject(vector, "owner", "knowledge-service");
   cJSON_AddBoolToObject(vector, "available", available ? 1 : 0);
   cJSON_AddStringToObject(vector, "backend", "pgvector");
   cJSON_AddStringToObject(vector, "kb_collection", PGVEC_KB_TABLE);
   cJSON_AddBoolToObject(vector, "kb_collection_ready", kb_ready > 0);

   if (kb_ready > 0)
      cJSON_AddNumberToObject(vector, "kb_points", (double)pgvec_point_count(PGVEC_KB_TABLE));

   if (!available)
   {
      cJSON_AddStringToObject(vector, "status", "unavailable");
      cJSON_AddStringToObject(vector, "message", "pgvector tables not accessible");
   }
   else if (kb_ready <= 0)
   {
      cJSON_AddStringToObject(vector, "status", "degraded");
      cJSON_AddStringToObject(vector, "message", "pgvector table exists but its ANN index is missing");
   }
   else
   {
      cJSON_AddStringToObject(vector, "status", "ok");
      cJSON_AddStringToObject(vector, "message", "pgvector ready");
   }

   return vector;
}
