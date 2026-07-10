/* hashline_edit.h: PURE transactional splice/plan core for anchor edits.
 *
 * Given an as-read snapshot (hashline_anchor) and a batch of anchor ops, verify
 * every op against the snapshot, plan the batch (reject conflicting/ambiguous
 * adjacencies), and produce a NEW file image with byte preservation: unchanged
 * lines keep their original bytes verbatim (their exact terminator); only edited
 * regions are normalized to the file's dominant line terminator. All-or-nothing:
 * one failing op rejects the whole batch with a precise reason and contested
 * range. This module is pure (bytes in, bytes out) and does no I/O, no snapshot
 * store access, no writes — the caller loads the file, resolves the snapshot,
 * parses ops, and performs the gated write. Leaf deps: libc + hashline_anchor.
 */
#ifndef DEC_HASHLINE_EDIT_H
#define DEC_HASHLINE_EDIT_H

#include "hashline_anchor.h"

#include <stddef.h>

typedef enum
{
   HL_OP_REPLACE,       /* replace line `at` with `text` */
   HL_OP_REPLACE_RANGE, /* replace lines [from..to] with `text` */
   HL_OP_INSERT_AFTER,  /* insert `text` as new line(s) after line `at` */
   HL_OP_DELETE_RANGE   /* delete lines [from..to] */
} hl_op_kind_t;

typedef struct
{
   hl_op_kind_t kind;
   int from;         /* 1-based ordinal (the `at` for replace/insert_after) */
   int to;           /* 1-based ordinal for ranges; ignored otherwise */
   char from_tag[8]; /* display tag parsed from the "N:tag" anchor (may be "") */
   char to_tag[8];   /* display tag for the range end anchor (may be "") */
   const char *text; /* replacement/insert text (borrowed); NULL/"" for delete */
} hl_edit_op_t;

typedef enum
{
   HL_EDIT_OK = 0,
   HL_EDIT_STALE,    /* file diverged from snapshot, or an anchor tag mismatched */
   HL_EDIT_CONFLICT, /* planning-time conflict between ops */
   HL_EDIT_BADOP,    /* malformed op or ordinal out of range */
   HL_EDIT_OOM
} hl_edit_status_t;

typedef struct
{
   int failed_op;      /* index of the first failing op, or -1 */
   const char *reason; /* static slug; see hashline_edit.c */
   int ctx_start;      /* 1-based contested line range (inclusive), or 0/0 */
   int ctx_end;
} hl_edit_fail_t;

/* Verify + plan + apply `ops` against `content` using `snap` for verification.
 * On HL_EDIT_OK, out_content and out_len receive a newly-allocated new file image
 * (caller frees). On any error, returns the status and fills *fail; out_content
 * is left NULL. `content` must match the snapshot (whole-file digest); a mismatch
 * yields HL_EDIT_STALE. */
hl_edit_status_t hashline_edit_apply(const char *content, size_t len,
                                     const hashline_snapshot_view_t *snap, const hl_edit_op_t *ops,
                                     size_t nops, char **out_content, size_t *out_len,
                                     hl_edit_fail_t *fail);

#endif /* DEC_HASHLINE_EDIT_H */
