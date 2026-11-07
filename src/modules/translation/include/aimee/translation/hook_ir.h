#ifndef AIMEE_TRANSLATION_HOOK_IR_H
#define AIMEE_TRANSLATION_HOOK_IR_H
#include <aimee/ir/aimee_ir.h>
#include <stddef.h>

/* Native hooks are policy/control inputs. Their payload ends at this adapter;
 * arguments and results thereafter use the canonical block representation.
 * Some legacy hook producers omit tool IDs. Such an input may be checked by
 * policy but must not be published as a model message or paired history event.
 * The adapter never invents an ID or infers an owning namespace from a name. */
typedef struct
{
   aimee_block_t call, result;
   char *session_id, *cwd, *tool_cwd;
} aimee_hook_ir_t;
#define AIMEE_HOOK_FRAME_MAX (4u * 1024u * 1024u)
int aimee_hook_ir_parse(const char *frame, size_t length, aimee_hook_ir_t *out);
/* Server IPC admission. A marked canonical hook cannot fall back to legacy
 * fields. Unmarked legacy envelopes convert only at this explicit entry. */
int aimee_hook_ir_admit(const struct cJSON *request, aimee_hook_ir_t *out);
/* Canonical hook intent, not a publishable message. Scope/auth stays in the
 * transport envelope. The result remains structured and carries is_error. */
struct cJSON *aimee_hook_ir_json(const aimee_hook_ir_t *hook);
void aimee_hook_ir_free(aimee_hook_ir_t *hook);
/* Arguments at existing policy/CLI output boundaries: native string arguments
 * remain strings; structured JSON is serialized without numeric truncation. */
char *aimee_hook_ir_arguments(const aimee_hook_ir_t *hook);
#endif
