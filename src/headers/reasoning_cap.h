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

#endif /* DEC_REASONING_CAP_H */
