#include "vector_verify.h"
#include "pgvec_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PGVEC_SCHEMA_VERSION = "v4";

const char *pgvec_schema_version(void)
{
   return PGVEC_SCHEMA_VERSION;
}

int pgvec_verify_snapshot(pgvec_verify_snapshot_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));

   snprintf(out->memory_collection, sizeof(out->memory_collection), "%s", PGVEC_MEMORY_TABLE);
   snprintf(out->kb_collection, sizeof(out->kb_collection), "%s", PGVEC_KB_TABLE);
   snprintf(out->server_version, sizeof(out->server_version), "pgvector");

   out->memory_exists = pgvec_table_ready(PGVEC_MEMORY_TABLE);
   out->kb_exists = pgvec_table_ready(PGVEC_KB_TABLE);
   out->memory_points = out->memory_exists > 0 ? pgvec_point_count(PGVEC_MEMORY_TABLE) : -1;
   out->kb_points = out->kb_exists > 0 ? pgvec_point_count(PGVEC_KB_TABLE) : -1;

   /* Indexed fields: report the btree index columns for the two tables. */
   out->memory_indexed_fields = strdup("record_type,primary_scope,workspace,project,kind");
   out->kb_indexed_fields = strdup("project");

   return 0;
}

void pgvec_verify_snapshot_cleanup(pgvec_verify_snapshot_t *snapshot)
{
   if (!snapshot)
      return;
   free(snapshot->memory_indexed_fields);
   snapshot->memory_indexed_fields = NULL;
   free(snapshot->kb_indexed_fields);
   snapshot->kb_indexed_fields = NULL;
}
