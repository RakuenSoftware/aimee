/* wfe_externalization.h -- the pre-delivery externalization guard (I3).
 *
 * An `enforced` workflow's whole promise is that nothing crosses the trust
 * boundary out of the work-item's worktree until the change has been reviewed
 * and roundtabled -- i.e. until gate.deliver has passed. This module is the pure
 * policy core of that guard: a versioned denylist of externalization primitives
 * plus a run-state predicate. A delegate running in an enforced run whose
 * gate.deliver has NOT advanced is denied every externalization primitive; once
 * delivered, the guard lifts. Per-block `allowed_tools` narrowing (S2) layers on
 * TOP of this baseline -- it never widens it.
 *
 * The predicate matches by canonical tool NAME (same layer gateway_policy strips
 * at). KNOWN LIMITATION: externalization performed THROUGH a general shell tool
 * (e.g. a Bash tool running `git push` / `curl`) is not caught by name-matching;
 * blocking that needs command-level inspection and is tracked as a follow-on
 * hardening (see the initiative proposal). The denylist is deliberately a
 * default-DENY set that requires positive review to extend (do not silently
 * remove entries).
 *
 * Design per the I1/I3 roundtable consult (2026-07-01), forks Q3 #24/#25/#27.
 */
#ifndef DEC_WFE_EXTERNALIZATION_H
#define DEC_WFE_EXTERNALIZATION_H 1

/* Bump when the denylist changes so audit tooling can pin which set was in
 * effect for a given run. */
#define WFE_EXTERNALIZATION_DENYLIST_VERSION 1

/* 1 if `tool_name` (canonical) is an externalization primitive -- it can cross
 * the trust boundary out of the worktree (open/merge a PR, push a ref, egress
 * the network, drive an MCP side effect, post a comment, edit CI, notify). */
int wfe_is_externalization_tool(const char *tool_name);

/* The guard: may `tool_name` be used by a delegate in a run with the given
 * delivery state? Non-externalization tools are always permitted here (per-block
 * narrowing is a separate, later layer). Externalization tools are permitted
 * ONLY once `delivered` (gate.deliver advanced for this run). NULL name -> denied
 * when not delivered (fail closed). */
int wfe_externalization_tool_permitted(const char *tool_name, int delivered);

/* 1 if `tool_name` is a DELIVER PRIMITIVE -- an action that transitions a
 * work-item's run state to delivered/accepted or makes its work visible/accepted
 * outside the gated run (pr.open, pr.merge, accept, mark-done, deploy, publish,
 * release, issue close). This is the CLOSED set the S2 gate.deliver enforcement
 * refuses pre-delivery, and that the per-block tool-strip removes pre-gate -- one
 * predicate, two enforcement points (consult Q3). It is a subset of the broader
 * externalization denylist above (deliver primitives all externalize, but not
 * every externalization is a delivery -- e.g. a bare `git push`). */
int wfe_is_deliver_primitive(const char *tool_name);

#endif /* DEC_WFE_EXTERNALIZATION_H */
