/* Tenant-scoped first-write generation lease for future non-empty registries. */
#ifndef DEC_ECONOMIZER_DISPATCH_LEASE_H
#define DEC_ECONOMIZER_DISPATCH_LEASE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct econ_dispatch_state econ_dispatch_state_t;

   typedef struct
   {
      uint64_t tenant_id;
      uint64_t account_id;
      uint64_t registry_generation;
      uint64_t pricing_generation;
      uint64_t contract_generation;
      uint64_t tokenizer_generation;
      uint64_t cohort_generation;
      uint64_t kill_switch_generation;
      int enabled;
   } econ_dispatch_facts_t;

   typedef struct
   {
      econ_dispatch_state_t *state;
      int held;
   } econ_dispatch_lease_t;

#define ECON_DISPATCH_LEASE_INIT {0}

   int econ_dispatch_state_create(const econ_dispatch_facts_t *initial,
                                  econ_dispatch_state_t **out);
   /* Destroy only after all replacement callers and read leases have joined. */
   void econ_dispatch_state_destroy(econ_dispatch_state_t *state);

   /* Replacement takes the write lock. Generation rollback and identity change
    * are rejected; disablement may accompany generation increments. */
   int econ_dispatch_state_replace(econ_dispatch_state_t *state, const econ_dispatch_facts_t *next);

   /* Returns 1 with a held read lease when every fact matches and is enabled;
    * returns 0 without a lease for pristine pass-through; -1 for misuse. Pass a
    * lease initialized with ECON_DISPATCH_LEASE_INIT. The
    * transport must hold a successful lease through its first outbound write. */
   int econ_dispatch_lease_begin(econ_dispatch_state_t *state,
                                 const econ_dispatch_facts_t *expected,
                                 econ_dispatch_lease_t *lease);
   void econ_dispatch_lease_end(econ_dispatch_lease_t *lease);

#ifdef __cplusplus
}
#endif

#endif
