#ifndef AIMEE_DB2_CODE_AUDIT_GRAPH_H
#define AIMEE_DB2_CODE_AUDIT_GRAPH_H

typedef struct
{
   const char *from;
   const char *to;
} audit_edge_t;

int code_audit_dead_exports(const char *const *exports, int n_exports, const char *const *imports,
                            int n_imports, const char **out, int max);
int code_audit_find_cycles(const audit_edge_t *edges, int n_edges, char **out, int max);

#endif
