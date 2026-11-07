#ifndef AIMEE_KB_INDEX_OBSERVER_H
#define AIMEE_KB_INDEX_OBSERVER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
   /* Generic source-owner publication lifecycle. The observer owns transport and
    * its opaque handle; the SQL owner supplies only actual committed source data.
    * Callbacks return zero on success. No callback may mutate the SQL source. */
   typedef struct aimee_kb_index_observer
   {
      unsigned abi_version; /* exactly 1 */
      int (*available)(void *context);
      int (*begin)(void *context, const char *project, int64_t generation, const char *operation,
                   void **handle_out);
      int (*file)(void *context, void *handle, const char *path, const char *utf8, size_t length);
      int (*ready)(void *context, void *handle, size_t complete_file_count);
      /* Missing/detached/failed snapshots MUST NOT be published ready. */
      int (*unavailable)(void *context, void *handle, const char *state);
      /* Always called once for every non-null handle, including failed scopes.
       * Frees transient observer state; must not publish a ready snapshot. */
      void (*release)(void *context, void *handle);
   } aimee_kb_index_observer_t;
   /* One-way for this process: set before source initialization when selected.
    * Missing/unavailable callbacks thereafter refuse source writes/startup. */
   void aimee_kb_index_observer_require(void);
   int aimee_kb_index_observer_required(void);
   /* Copy registration. NULL unregisters only with no active scope; requirement
    * remains set. The caller must not unload callback code while registered. */
   int aimee_kb_index_observer_register(const aimee_kb_index_observer_t *observer, void *context);
#ifdef __cplusplus
}
#endif
#endif
