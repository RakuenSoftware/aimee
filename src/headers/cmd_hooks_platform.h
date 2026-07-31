/* cmd_hooks_platform.h: shared declarations for cmd_hooks.c platform split. */
#ifndef DEC_CMD_HOOKS_PLATFORM_H
#define DEC_CMD_HOOKS_PLATFORM_H 1

#include "aimee.h"
#include "config.h"

/* Returns 1 if stdin has data ready to read (non-blocking), 0 otherwise. */
int platform_hooks_stdin_ready(void);

/* Fork a background child (POSIX) or run synchronously (Windows) to re-index
 * the given projects. idx_names, idx_roots, and idx_heads are arrays of
 * idx_count entries; idx_heads is the main HEAD that becomes authoritative only
 * after the KB scan succeeds. */
void platform_hooks_background_reindex(char idx_names[][128], char idx_roots[][MAX_PATH_LEN],
                                       char idx_heads[][64], int idx_count);

/* Defined in cmd_hooks.c; called by platform_hooks_background_cleanup. */
void prune_stale_sessions(void);

/* Fork a background child (POSIX) or run synchronously (Windows) to prune
 * stale sessions using cfg->db1_path. */
void platform_hooks_background_cleanup(void);

#endif /* DEC_CMD_HOOKS_PLATFORM_H */
