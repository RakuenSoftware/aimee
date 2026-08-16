#ifndef DEC_DB2_VECTOR_VERIFY_H
#define DEC_DB2_VECTOR_VERIFY_H 1

#include <stdint.h>

typedef struct
{
   char server_version[64];
   char memory_collection[64];
   char kb_collection[64];
   int memory_exists;
   int kb_exists;
   int64_t memory_points;
   int64_t kb_points;
   char *memory_indexed_fields;
   char *kb_indexed_fields;
} pgvec_verify_snapshot_t;

const char *pgvec_schema_version(void);
int pgvec_verify_snapshot(pgvec_verify_snapshot_t *out);
void pgvec_verify_snapshot_cleanup(pgvec_verify_snapshot_t *snapshot);

#endif /* DEC_DB2_VECTOR_VERIFY_H */
