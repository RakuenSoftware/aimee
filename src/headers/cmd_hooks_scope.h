#ifndef DEC_CMD_HOOKS_SCOPE_H
#define DEC_CMD_HOOKS_SCOPE_H 1

/* Scope-aware section builder shared between cmd_hooks (pre/post hook
 * context injection) and cmd_session_start (session-start context).
 *
 * Both surfaces need to render a block of memories filtered by workspace /
 * project visibility, sorted by scope rank and insertion order. The logic
 * and the per-item buffer type are identical — kept in one place so the two
 * callers can't drift. */

#include <stddef.h>

struct cJSON;

/* Resolve the workspace and project labels that apply to |cwd|.  Each may
 * come back as an empty string when no configured workspace contains |cwd|
 * or the project cannot be derived. */
void hook_scope_labels_for_cwd(const char *cwd, char *workspace_out, size_t workspace_len,
                               char *project_out, size_t project_len);

#endif /* DEC_CMD_HOOKS_SCOPE_H */
