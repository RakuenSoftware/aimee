#ifndef DEC_DELEGATE_SANDBOX_IMAGE_H
#define DEC_DELEGATE_SANDBOX_IMAGE_H

#include <stddef.h>

/* Resolve the delegate-sandbox image for a delegate whose worktree/cwd is `cwd`.
 *
 * Precedence, most specific first:
 *   1. the repo's <git-root>/.aimee/project.yaml `sandbox.image` (travels with the
 *      project — the code declares its own toolchain image);
 *   2. a per-workspace `sandbox_image` override in aimee.yaml (the workspace whose
 *      root contains `cwd`; the longest matching root wins);
 *   3. the global `delegate_sandbox_image` default in aimee.yaml.
 *
 * Writes the image reference into out[cap] and returns 0 on a hit, or -1 if none is
 * configured — in which case the caller runs the backend default image. Only the
 * `image:` (pre-baked) form is resolved here; the build-from-spec form is a later
 * phase. */
int delegate_sandbox_resolve_image(const char *cwd, char *out, size_t cap);

#endif /* DEC_DELEGATE_SANDBOX_IMAGE_H */
