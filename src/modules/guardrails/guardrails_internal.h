/* guardrails_internal.h: helpers shared across the guardrails module
 * (guardrails.c, guardrails_orchestrator.c, guardrails_tdd.c).
 *
 * Not a public API — callers outside the module should use
 * `guardrails.h`. These are kept out of the public header to preserve
 * the module's surface. */
#ifndef DEC_GUARDRAILS_INTERNAL_H
#define DEC_GUARDRAILS_INTERNAL_H 1

#include "aimee.h"
#include <stddef.h>

/* Does `tool` name a file-edit tool (Write/Edit/MultiEdit/write_file)?
 * Defined in guardrails_orchestrator.c; used from guardrails_tdd.c. */
int is_edit_tool(const char *tool);

/* Return the basename of `path` (pointer into the same buffer; no alloc).
 * Defined in guardrails.c; used from guardrails_orchestrator.c. */
const char *basename_of(const char *path);

/* TDD gate: refuse a write when the target is a source file whose
 * corresponding test file hasn't been edited this session. Returns 0 to
 * allow, 1 to block (caller formats a message into msg_buf).
 * Defined in guardrails_tdd.c; called from pre_tool_check. */
int tdd_check_impl_write(const session_state_t *state, const char *path, char *msg_buf,
                         size_t msg_len);

#endif /* DEC_GUARDRAILS_INTERNAL_H */
