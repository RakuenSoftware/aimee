/* delegate_backend_local.h: local subprocess execution backend.
 *
 * First real backend on the delegate_backend_t interface. Workspace
 * lives under ~/.cache/aimee/delegate/<task_id>/. Acquire creates it,
 * release with hibernate=0 removes it, release with hibernate=1 keeps
 * it so the next acquire(same task_id) resumes the same workspace.
 *
 * exec() forks bash -c <command> in the workspace cwd, captures
 * stdout/stderr into the caller-provided result buffers. File I/O,
 * cwd, and hibernated workspace reuse are implemented by this backend. */
#ifndef DEC_DELEGATE_BACKEND_LOCAL_H
#define DEC_DELEGATE_BACKEND_LOCAL_H 1

#include "delegate_backend.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Register the local backend in the process-global registry under
    * the name "local". Called once at server start. Returns 0 on
    * success, -1 on duplicate-name collision (the existing entry
    * wins). Idempotent in the sense that a second call after a
    * successful first call is a no-op returning -1. */
   int delegate_backend_register_local(void);

   /* Test seam: lookup the static `delegate_backend_local` instance
    * directly (without going through the registry). Used by unit tests
    * to exercise the vtable without polluting registry state across
    * cases. Production code should always use delegate_backend_lookup
    * after register_local. */
   delegate_backend_t *delegate_backend_local_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DELEGATE_BACKEND_LOCAL_H */
