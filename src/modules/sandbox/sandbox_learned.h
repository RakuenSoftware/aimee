#ifndef DEC_SANDBOX_LEARNED_H
#define DEC_SANDBOX_LEARNED_H

/* sandbox_learned: the "learned toolchain" for delegate sandboxes.
 *
 * A delegate sandbox is `--network none` by intent; its toolchain must be baked into
 * the image at build time. Rather than make every project author a spec, aimee learns
 * what a project actually needed: when a delegate runs `apt-get install <pkgs>` inside
 * its sandbox, aimee captures the package names (the intent) and records them against
 * the project (its git root). The next time that project's sandbox image is built, the
 * learned set is pre-baked in, so the tools are present immediately with no runtime
 * fetch — closing the loop without any runtime network.
 *
 * The store is a JSON sidecar under AIMEE_HOME (`sandbox-learned.json`):
 *   { "<git-root>": ["gcc", "make", "libssl-dev"], ... }
 *
 * This is best-effort: parsing shell commands is heuristic (it only recognises apt,
 * matching the apt-based image builder), and a learned build that fails to build must
 * fall back to the un-augmented image — never break a delegate. */

#include <stddef.h>

#define SBX_PKG_MAX   64  /* max apt package-name length (Debian caps at ~ this) */
#define SBX_LEARN_MAX 128 /* max learned packages retained per project */

/* Parse apt-install package names out of a shell command. Recognises `apt install`
 * and `apt-get install` (optionally behind `sudo`), collects the package-name tokens
 * that follow (skipping flags and stopping at a shell operator && || | ; & newline),
 * and validates each against the Debian package-name grammar so no shell metacharacter
 * or path can be recorded. De-dupes within the call. Writes up to `max` names into
 * out[][SBX_PKG_MAX] and returns the count (>=0). Pure; exposed for unit tests. */
int sandbox_learned_parse_apt(const char *cmd, char out[][SBX_PKG_MAX], int max);

/* Load the learned package set for `git_root` into out[][SBX_PKG_MAX]. Returns the
 * count (>=0), or 0 if none/unreadable. Sorted for a deterministic build hash. */
int sandbox_learned_load(const char *git_root, char out[][SBX_PKG_MAX], int max);

/* Merge `pkgs` (n names) into the learned set for `git_root` and persist. New names
 * are unioned with the existing set (capped at SBX_LEARN_MAX, oldest-insertion order
 * preserved). A no-op when n==0 or nothing is new. Returns 0 on success, -1 on error. */
int sandbox_learned_record(const char *git_root, const char *const *pkgs, int n);

/* Convenience for the capture sites: parse `cmd` for apt installs and, if any are
 * found, record them against the git root that contains `cwd`. Silent best-effort
 * (never fails a delegate turn); does nothing if `cwd` is not in a git repo. */
void sandbox_learned_observe(const char *cwd, const char *cmd);

#endif /* DEC_SANDBOX_LEARNED_H */
