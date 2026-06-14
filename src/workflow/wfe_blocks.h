/* wfe_blocks.h: the non-gate block executors (author/implement/freeze/pr/merge)
 * and a few standalone, unit-testable helpers (git freeze). Registered behind
 * the wfe_iface vtable via wfe_register_default_executors(). */
#ifndef DEC_WFE_BLOCKS_H
#define DEC_WFE_BLOCKS_H 1

#include <stddef.h>

/* Register the real executors for every non-gate block type. Gates are
 * registered by their own slices (W4 human, W5 roundtable). */
void wfe_register_default_executors(void);

/* Compute a frozen diff for the implementation gate. base_ref defaults to the
 * merge-base of HEAD against `base_branch` (e.g. "origin/testing" or "main").
 * Fills the short SHAs and the sha256 hex of the cumulative diff. Returns 0 on
 * success. Standalone (no engine ctx) so it is directly unit-testable. */
int wfe_git_freeze(const char *repo_dir, const char *base_branch, char out_base_sha[64],
                   char out_head_sha[64], char out_diff_hash[65], char *err, size_t errlen);

#endif /* DEC_WFE_BLOCKS_H */
