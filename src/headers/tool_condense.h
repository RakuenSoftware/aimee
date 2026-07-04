/* tool_condense.h: deterministic command-aware tool-output condensation primitives
 * (proposal: deterministic-tool-output-condensation), Slice 1.
 *
 * Pure, deterministic, no LLM. Each primitive returns a NEW malloc'd NUL-terminated
 * string the caller frees, or NULL on allocation failure — a NULL is the caller's cue
 * to fall back to the raw input (fail-open; condensation never loses the output). The
 * primitives are the toolbox the per-command family rules (later slices) compose from;
 * no tool seam is wired in this slice, so the lever is inert until then. */
#ifndef AIMEE_TOOL_CONDENSE_H
#define AIMEE_TOOL_CONDENSE_H

#include "config.h"

/* 1 iff the command-filter lever is enabled (reduce_command_filter). */
int tool_condense_enabled(const config_t *cfg);

/* Strip content-free noise, line-wise:
 *  - remove ANSI CSI escape sequences (ESC '[' … final-byte);
 *  - resolve carriage-return progress redraws (keep only the text after the last '\r'
 *    on a line — the final rendered state);
 *  - collapse a run of 2+ blank lines to a single blank line.
 * Never drops a line that carries text. */
char *tc_strip_noise(const char *in);

/* Collapse a run of immediately-repeated identical lines to one line followed by
 * "  (xN)" (N = the repeat count). Non-repeated lines pass through unchanged. */
char *tc_dedup_lines(const char *in);

/* Keep the first `head` and last `tail` lines, and — in between — every line that
 * contains `signal` (case-sensitive substring; NULL = none forced). Each elided run is
 * replaced by a single "... N lines elided ..." marker so the elision is explicit.
 * head/tail < 0 are treated as 0. If the input already fits (head+tail >= line count),
 * it is returned verbatim. */
char *tc_truncate_with_signal(const char *in, int head, int tail, const char *signal);

#endif /* AIMEE_TOOL_CONDENSE_H */
