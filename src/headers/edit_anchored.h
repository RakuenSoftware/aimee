/* edit_anchored.h: transactional, anchor-based edit planner.
 * (proposal: hashline-edit-and-lean-websearch, Part I — edit verbs.)
 *
 * The planner is PURE: given the current file bytes, the read snapshot the
 * model's anchors came from, and an edits[] array, it either produces the new
 * file text (all-or-nothing) or a structured rejection (stale anchors /
 * validation / interaction conflicts) — never a partial apply, never any I/O.
 * The dispatch layer owns file reads, write guards, write-back, and minting the
 * fresh snapshot for a rejection's re-anchor context. */
#ifndef AIMEE_EDIT_ANCHORED_H
#define AIMEE_EDIT_ANCHORED_H

#include "anchor_snapshot.h"
#include "cJSON.h"
#include <stddef.h>

typedef struct
{
   char *new_text; /* success: malloc'd new file content (caller frees) */
   cJSON *reject;  /* rejection: caller owns; caller injects "snapshot_id" then prints */
} edit_anchored_result_t;

/* Plan a batch of anchored edits against `cur_bytes` using `snap`'s recorded
 * per-line digests. `edits` is the JSON array from the tool call.
 *
 * Returns:
 *   0  success           -> res->new_text set (res->reject NULL)
 *   1  rejected          -> res->reject set   (res->new_text NULL); the caller
 *                           adds a fresh "snapshot_id" and returns it verbatim
 *  -1  internal error    -> res->reject set to a {"status":"error",...} object
 *
 * On a stale-anchor rejection the reject object already carries a "context"
 * array of re-anchored current lines around the contested range. */
int edit_anchored_plan(const char *cur_bytes, size_t cur_len, const anchor_snapshot_t *snap,
                       cJSON *edits, edit_anchored_result_t *res);

#endif /* AIMEE_EDIT_ANCHORED_H */
