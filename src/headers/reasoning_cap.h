/* reasoning_cap.h: deterministic complexity score -> reasoning-effort cap.
 *
 * Proposal §5. A pure, deterministic 0-10 complexity score (derived from turn
 * size and tool presence) is used only to *cap* the reasoning effort of a
 * request — it never routes (routing is the bandit, §6) and never raises a
 * configured effort. The cap only lowers an explicitly configured effort on
 * low-complexity turns; an empty/unknown effort (provider default) is returned
 * unchanged so the cap can never silently impose — or raise — effort. The
 * caller is responsible for only applying this on provider surfaces that accept
 * a reasoning-effort enum (Codex/OpenAI-compatible, Claude --effort); unsupported
 * providers must not be handed a reasoning field. */
#ifndef DEC_REASONING_CAP_H
#define DEC_REASONING_CAP_H 1

#include <stddef.h>

/* Deterministic 0-10 complexity score from message count, total user content
 * length (bytes), and whether the turn carries tools. Higher = more complex. */
int reasoning_complexity_score(int message_count, size_t content_len, int has_tools);

/* Return the effort enum capped to the complexity ceiling implied by `score`:
 *   score 0-2  -> ceiling "low"
 *   score 3-5  -> ceiling "medium"
 *   score 6-8  -> ceiling "high"
 *   score 9-10 -> no ceiling (configured returned unchanged)
 * The result is `configured_effort` unless it ranks strictly above the ceiling,
 * in which case the ceiling string is returned. `configured_effort` that is
 * empty or not a recognised enum (low/medium/high/xhigh/max) is returned
 * verbatim — the cap only ever lowers an explicit setting. The returned pointer
 * is either `configured_effort` itself or a static string literal. */
const char *reasoning_effort_capped(const char *configured_effort, int score);

#endif /* DEC_REASONING_CAP_H */
