/* Remaining native ontology-review lookup. Fact ingestion is owned by Go. */
#ifndef DEC_DB2_REL_TYPES_STORE_H
#define DEC_DB2_REL_TYPES_STORE_H 1
#ifdef __cplusplus
extern "C"
{
#endif
   int db2_rel_types_resolve(const char *rel_type, long *out_id);
#ifdef __cplusplus
}
#endif
#endif
