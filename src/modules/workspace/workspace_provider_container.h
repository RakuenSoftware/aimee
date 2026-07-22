#ifndef WORKSPACE_PROVIDER_CONTAINER_H
#define WORKSPACE_PROVIDER_CONTAINER_H 1

#include "delegate_backend.h"
#include "workspace_provider.h"

/* Container workspace provider: file/exec for a delegate that runs inside its own
 * container, marshalled through a delegate backend (normally `docker`).
 *
 * This is the seam the delegate sandbox needs, and it already existed — the
 * workspace_provider header has said so from the start:
 *
 *   "To let aimee-server run detached (e.g. in a container) the proposal routes
 *    file/exec access through a pluggable provider selected per workspace."
 *
 * td_bash / td_read_file / td_write_file / td_list_files / execute_script already
 * resolve through workspace_provider_active(). So a delegate whose turn binds one
 * of these runs its shell and its file ops INSIDE the container, with no change to
 * the dispatch at all. Without it, td_bash falls through to run_cmd — in-process,
 * inside aimee-server, with the server's filesystem and environment. That is the
 * gap the sandbox proposal names.
 *
 * The provider stays the narrow boundary the contract describes: raw,
 * already-resolved I/O only. Path resolution, the write guards and output shaping
 * stay above it, identical for every provider.
 *
 * NOT resolvable from config by design — see WS_PROVIDER_CONTAINER in
 * workspace_provider.h. An instance needs a live container handle, so it is bound
 * per delegate; a config-selectable kind would have to fall back to `shared` when
 * no container exists, which would silently run the delegate on the host — the one
 * outcome the sandbox exists to prevent. */

typedef struct
{
   workspace_provider_t base; /* first: &inst.base casts to workspace_provider_t* */
   delegate_backend_t *backend;
   void *state; /* the backend's per-workspace handle from acquire() */
} ws_container_provider_t;

/* Bind `out` to `backend`/`state` (both required; `state` is what the backend's
 * acquire() produced). Afterwards &out->base is a workspace_provider_t* whose ops
 * run inside that container. Returns 0, or -1 if either argument is NULL — a
 * half-initialized provider must never be handed out, because every op would fall
 * through to the host.
 *
 * The caller owns the container lifecycle (acquire/release); this only borrows the
 * handle for the turn, mirroring workspace_provider_set_active/clear_active. */
int ws_container_provider_init(ws_container_provider_t *out, delegate_backend_t *backend,
                               void *state);

#endif /* WORKSPACE_PROVIDER_CONTAINER_H */
