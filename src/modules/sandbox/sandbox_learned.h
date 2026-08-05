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
 * The parser and the JSON store now live in the sandbox MODULE
 * (server-go/modules/sandbox), reached over the event bus. That is where they belong:
 * parsing an untrusted shell command and owning a sidecar file is feature work, and
 * the C core carries messages rather than performing it. What stays here is the part
 * that is genuinely C's: the cheap prefilter, the config gate, and resolving the git
 * root of a working directory.
 *
 * This is best-effort: parsing shell commands is heuristic (it only recognises apt,
 * matching the apt-based image builder), and a learned build that fails to build must
 * fall back to the un-augmented image — never break a delegate. */

#include <stddef.h>

#define SBX_PKG_MAX   64  /* max apt package-name length (Debian caps at ~ this) */
#define SBX_LEARN_MAX 128 /* max learned packages retained per project */

/* Load the learned package set for `git_root` into out[][SBX_PKG_MAX]. Returns the
 * count (>=0), or 0 if none/unreadable. Sorted by the module for a deterministic
 * build hash. */
int sandbox_learned_load(const char *git_root, char out[][SBX_PKG_MAX], int max);

/* Parse `cmd` for apt installs and, if any are found, record them against the git root
 * that contains `cwd`. Silent best-effort (never fails a delegate turn); does nothing
 * if `cwd` is not in a git repo, if learning is disabled, or if the module is not
 * attached. */
void sandbox_learned_observe(const char *cwd, const char *cmd);

#endif /* DEC_SANDBOX_LEARNED_H */
