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

/* --- cache management (aimee-sbx:* images accumulate; these enumerate + prune) --- */

/* Parse a `docker image ls` CreatedAt string ("2026-07-15 12:34:56 +0000 UTC") to a
 * UTC epoch (seconds). Returns 0 on success, -1 if it does not parse. Pure; exposed
 * for unit tests. */
int delegate_sandbox_parse_created_epoch(const char *created, long long *out);

/* The gc keep/remove decision for one image, factored out so it can be tested without
 * a docker daemon. `index` is the image's position in the newest-first ordering.
 * Returns 1 to remove, 0 to keep, and sets *reason_out (static string) to one of
 * "in-use" | "kept-recent" | "within-max-age" | "aged-out". An image is removed only
 * when it is not in use, is beyond the `keep_min` most-recent, and (if its
 * created_epoch is known, i.e. > 0) is at least `max_age_secs` old. A zero/unknown
 * created_epoch is treated as old enough to remove. Pure. */
int delegate_sandbox_gc_should_remove(int in_use, int index, int keep_min, long long created_epoch,
                                      long long now, long max_age_secs, const char **reason_out);

/* List every build-from-spec image (tag prefix `aimee-sbx:`) as a JSON array
 * (caller frees). Each element:
 *   {"tag":"aimee-sbx:ab12cd34ef56","id":"<12hex>","created":"2026-07-15 12:34:56 +0000 UTC",
 *    "size":"142MB","in_use":false}
 * `in_use` is true when a container (running or stopped) still references the tag.
 * Returns malloc'd "[]" when none exist, or NULL if the docker daemon is unreachable. */
char *delegate_sandbox_images_json(void);

/* Garbage-collect build-from-spec images. Removes each `aimee-sbx:*` image that is
 * (a) not referenced by any container and (b) strictly older than `max_age_secs`
 * (by image CreatedAt) — EXCEPT the `keep_min` most-recently-created images, which are
 * always retained regardless of age. `max_age_secs <= 0` means "any age" (0s floor);
 * `keep_min < 0` is treated as 0.
 *
 * When `dry_run` is non-zero nothing is removed; the report lists what WOULD go.
 * `*report_json_out` (caller frees, may be NULL to skip) receives:
 *   {"removed":3,"kept":5,"dry_run":false,"images":[{"tag":...,"reason":"aged-out"|"in-use"|
 *    "kept-recent"|"within-max-age"},...]}
 * Returns 0 on success (even if nothing matched), -1 if the docker daemon is unreachable. */
int delegate_sandbox_gc(long max_age_secs, int keep_min, int dry_run, char **report_json_out);

#endif /* DEC_DELEGATE_SANDBOX_IMAGE_H */
