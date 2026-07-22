#ifndef DEC_ECONOMIZER_PROVENANCE_H
#define DEC_ECONOMIZER_PROVENANCE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct econ_provenance econ_provenance_t;

   typedef struct
   {
      uint64_t tenant_id;
      uint64_t task_id;
      uint64_t call_id;
      uint64_t semantic_contract_id;
      uint64_t transform_id;
      uint64_t transform_version;
   } econ_provenance_binding_t;

   /* Issuance is for the authenticated local-tool completion boundary only.
    * The opaque capability is process-local and is never serialized. */
   int econ_provenance_issue_local(const econ_provenance_binding_t *binding, const void *source,
                                   size_t source_len, econ_provenance_t **out);

   /* Atomically consume once. The binding and exact source bytes must match. */
   int econ_provenance_consume(econ_provenance_t *cap, const econ_provenance_binding_t *expected,
                               const void *source, size_t source_len);
   void econ_provenance_destroy(econ_provenance_t *cap);

#ifdef __cplusplus
}
#endif

#endif
