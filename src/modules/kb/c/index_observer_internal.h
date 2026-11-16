#ifndef AIMEE_KB_INDEX_OBSERVER_INTERNAL_H
#define AIMEE_KB_INDEX_OBSERVER_INTERNAL_H
#include "aimee/kb/index_observer.h"
typedef struct
{
   void *state;
   int finished;
} kb_index_mutation_t;
int kb_index_mutation_begin(kb_index_mutation_t *guard, void *connection, const char *project,
                            const char *operation, int may_reattach);
int kb_index_mutation_project_id(kb_index_mutation_t *guard, void *connection, int64_t project_id,
                                 const char *operation);
int kb_index_mutation_file_id(kb_index_mutation_t *guard, void *connection, int64_t file_id,
                              const char *operation);
int kb_index_mutation_finish(kb_index_mutation_t *guard);
void kb_index_mutation_cleanup(kb_index_mutation_t *guard);
int kb_index_observer_connection_closing(void *connection);
/* Selected startup requires a complete readable pre-migration project schema.
 * Bootstrap/old-schema installation must run offline before selection. */
int kb_index_observer_startup_begin(kb_index_mutation_t *guard, void *connection);
#define KB_INDEX_MUTATION(name)                                                                    \
   kb_index_mutation_t name __attribute__((cleanup(kb_index_mutation_cleanup))) = {0}
#endif
