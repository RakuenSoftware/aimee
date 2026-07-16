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

/* --- exposed for unit tests (pure helpers, no docker) --- */

/* Generate the RUN-layer Dockerfile for a `from` + `packages` spec into out[cap]:
 *   FROM <base>
 *   RUN apt-get update && apt-get install -y --no-install-recommends <pkgs> && ...
 * Each package name must match [A-Za-z0-9][A-Za-z0-9._+:-]* (no shell metacharacters
 * reach the build RUN). Returns 0 on success, -1 on an invalid package, empty base,
 * or truncation. */
int delegate_sandbox_dockerfile_from_packages(const char *base, const char *const *pkgs, int npkgs,
                                              char *out, size_t cap);

/* Deterministic content-addressed image tag `aimee-sbx:<12-hex>` for `content`
 * (the Dockerfile text). Same content -> same tag, so a built image is reused. */
void delegate_sandbox_content_tag(const char *content, char *tag, size_t cap);

#endif /* DEC_DELEGATE_SANDBOX_IMAGE_H */
