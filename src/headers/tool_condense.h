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

/* ---- command recognition (Slice 2) ---- */

/* The recognition outcome for a shell command line. Drives whether/how the output is
 * condensed and the ledger's recognized/unrecognized accounting. */
typedef enum
{
   TC_UNRECOGNIZED = 0, /* not a command we handle (or a compound/piped line) -> passthrough */
   TC_OPAQUE,           /* multiplexer/make/script: only a generic fallback may ever apply */
   TC_RECOGNIZED, /* a command we intend to condense; the FAMILY is resolved in later slices */
} tc_reco_t;

typedef struct
{
   tc_reco_t outcome;
   char cmd[64]; /* normalized inner command basename (after wrapper unwrapping), or "" */
   char sub[64]; /* the subcommand token (git <sub>, cargo <sub>, npm <sub>, …), or "" */
} tc_reco_result_t;

/* Recognize a shell command line: reject compound/piped/substituted lines (fail-open ->
 * UNRECOGNIZED), strip known single-command wrappers (env VAR=…, sudo, nice, time, npx,
 * bun x, pnpm/npm/yarn exec, uv/poetry/pipenv run), and classify the inner command.
 * xargs and make and scripts (./x, bash x, sh -c) are OPAQUE (never a family rule).
 * Deterministic + allocation-free (fixed output buffers). */
tc_reco_result_t tc_recognize(const char *cmdline);

/* ---- family filters + the top-level apply (Slice 3) ---- */

/* Test-runner family filter: keep the summary + every failure verbatim, drop passing-case
 * transcripts. `exit_code` biases safety — a non-zero exit with NO recognizable failure
 * lines returns NULL (verbatim passthrough, never hide the cause). Returns a NEW string
 * (caller frees), or NULL to pass the raw output through unchanged. */
char *tc_family_test_runner(int exit_code, const char *in);

/* Compiler/linter diagnostics family (Slice 5): keep errors/warnings/notes + file:line
 * diagnostics, drop progress chatter. A non-zero exit's error lines are always kept.
 * Returns a NEW string (caller frees) or NULL to pass through. */
char *tc_family_diagnostics(int exit_code, const char *in);

/* Per-condensation accounting for the economizer ledger. */
typedef struct
{
   long raw_bytes;   /* input size */
   long final_bytes; /* condensed size (== raw_bytes when passed through) */
   int recognized;   /* the command was RECOGNIZED */
   int spilled;      /* a full-output spill file was written */
   char family[24];  /* "test", "" (none), … */
   char spill_ref[40];
} tc_stats_t;

/* Top-level condensation entry (no seam wired yet — S4 calls this at the tool seam).
 * Recognizes `cmdline`; if a family rule materially shrinks `raw`, writes the FULL raw
 * output to a spill file under `spill_dir` and returns the condensed text with a trailing
 * "… full output: <ref>" pointer (caller frees). Returns NULL to pass `raw` through
 * unchanged (unrecognized / no gain / a filter or spill failure -> fail-open). `stats`
 * (optional) is filled for the ledger. `spill_dir` NULL disables spilling (then a
 * successful condense that would need a spill instead passes through — never lossy). */
char *tool_condense_apply(const config_t *cfg, const char *cmdline, int exit_code, const char *raw,
                          const char *spill_dir, tc_stats_t *stats);

/* ---- realized-savings observability (Slice 6) ---- */

/* Cumulative process-wide counters, updated at the seam so an operator can measure the
 * lever's REALIZED savings on real traffic before any default-on decision. */
typedef struct
{
   long long recognized;    /* recognized commands seen by tool_condense_apply */
   long long applied;       /* condensations that shrank + spilled (a family fired) */
   long long applied_raw;   /* total input bytes over APPLIED condensations */
   long long applied_final; /* total output bytes over APPLIED condensations */
   long long family_test;   /* applied condensations tagged "test" */
   long long family_diag;   /* applied condensations tagged "diag" */
} tool_condense_totals_t;

/* Snapshot the counters. Each field is read atomically, but the six are NOT a single
 * transactional point-in-time view (a concurrent condensation may land between reads) —
 * fine for monotonic metrics; don't assert cross-counter invariants on a live snapshot. */
void tool_condense_stats_snapshot(tool_condense_totals_t *out);
/* Reset the counters. TEST-ONLY: it races lost-updates with live traffic, so call it only
 * when no condensation is in flight (the unit test does). */
void tool_condense_stats_reset(void);

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
