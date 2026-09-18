#ifndef AIMEE_DB2_MEMORY_SCOPE_QUERY_H
#define AIMEE_DB2_MEMORY_SCOPE_QUERY_H

/* Transitional request context for native KB transports. Visibility and
 * candidate ordering are owned by the Go memory module. */
typedef struct
{
   int active;
   int include_all;
   char workspace[512];
   char project[512];
   char scope_type[64];
   char scope_value[512];
} db2_memory_scope_context_t;

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all);
void db2_memory_scope_context_set_exact(const char *workspace, const char *project,
                                        const char *scope_type, const char *scope_value,
                                        int include_all);
void db2_memory_scope_context_clear(void);
void db2_memory_scope_context_get(db2_memory_scope_context_t *out);

#endif
