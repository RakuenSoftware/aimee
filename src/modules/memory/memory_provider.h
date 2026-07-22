/* memory_provider.h: runtime ABI for pluggable memory providers.
 *
 * Part of the pluggable context engine surface described in
 * docs/proposals/pending/plugin-extension-surface-and-context-engine.md.
 *
 * The bundled aimee-kb provider is always registered at startup.
 * External providers (is_bundled=0) are capped at one:
 *   bundled + one external is the maximum.
 * A second external registration is rejected with LOG_WARN.
 *
 * Function pointer parameters that reference types not yet fully defined
 * use void* and will be tightened when the referenced types land.
 */
#ifndef DEC_MEMORY_PROVIDER_H
#define DEC_MEMORY_PROVIDER_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct memory_provider
   {
      const char *name;
      int is_bundled; /* 1 = bundled (aimee-kb); 0 = external */

      /* All function pointers are optional (NULL = not implemented). */

      /* Return 1 if this provider is ready (DB reachable, etc.). */
      int (*is_available)(struct memory_provider *self);

      /* Called once per session start.  args is session_init_args_t* when typed. */
      int (*initialize)(struct memory_provider *self, void *args);

      /* Write the memory block that goes into the system prompt. */
      int (*system_prompt_block)(struct memory_provider *self, char **out, size_t *cap);

      /* Fetch context for the upcoming user query.  out_context is heap-alloc'd; caller frees. */
      int (*prefetch)(struct memory_provider *self, const char *user_query, char **out_context);

      /* Record a completed turn.  user/asst are message_t* when typed. */
      int (*sync_turn)(struct memory_provider *self, const void *user, const void *asst);

      /* Called when the session ends. */
      int (*on_session_end)(struct memory_provider *self);

      /* Called before context compaction.  session_state is session_state_t* when typed.
       * Provider may write additional summary text into *extra_summary (heap-alloc'd; caller
       * frees). */
      int (*on_pre_compress)(struct memory_provider *self, const void *session_state,
                             char **extra_summary);

      /* Called when a memory write occurs.  write is memory_write_t* when typed. */
      int (*on_memory_write)(struct memory_provider *self, const void *write);

      /* Called when a delegation is recorded.  delegation is delegation_record_t* when typed. */
      int (*on_delegation)(struct memory_provider *self, const void *delegation);

      /* Teardown at process exit. */
      int (*shutdown)(struct memory_provider *self);

      /* Optional: return extra tools exposed by this provider (NULL = none). */
      const void *(*get_tools)(struct memory_provider *self, int *count);

      void *user_data; /* provider-private state */
   } memory_provider_t;

   /* Register a provider.
    *   is_bundled=1 : the bundled aimee-kb provider.  Always accepted; replaces any prior bundled.
    *   is_bundled=0 : an external provider.  At most one allowed.  Second attempt logs WARN,
    * returns -1. Returns 0 on success, -1 on rejection. */
   int memory_provider_register(const memory_provider_t *p);

   /* Return the active provider (external if registered, else bundled, else NULL). */
   const memory_provider_t *memory_provider_get_active(void);

   /* Return the bundled provider, or NULL if not yet registered. */
   const memory_provider_t *memory_provider_get_bundled(void);

   /* Return the external provider, or NULL if none registered. */
   const memory_provider_t *memory_provider_get_external(void);

   /* Reset the registry.  Only for use in tests. */
   void memory_provider_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_PROVIDER_H */
