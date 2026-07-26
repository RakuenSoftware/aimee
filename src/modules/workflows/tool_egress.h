/* tool_egress.h -- authoritative egress classification for built-in tools.
 *
 * WHY THIS EXISTS
 *
 * The workflow externalization gate used to decide "does this tool send data
 * out of the controlled boundary?" by matching the tool's NAME against a
 * hand-maintained list. That is fail-OPEN by construction: a tool added after
 * the list was written is silently ungated. This was not hypothetical --
 * `web_read`, which performs real outbound HTTP, was absent from both the
 * externalization deny-list and the native gate's web-tool list while its
 * externalized equivalents (`WebFetch`, `curl`) were denied.
 *
 * This module replaces the guess with a declaration. Every built-in tool must
 * appear here exactly once. `agent_tools_validate_egress_table()` enforces that
 * at startup against the real tool table, so ADDING A TOOL WITHOUT DECLARING
 * ITS EGRESS IS A STARTUP FAILURE, not a silent bypass.
 *
 * WHAT "EGRESS" MEANS HERE
 *
 * The gate's question is NOT "does this touch a socket?" -- it is "can this
 * carry data outside the trust boundary before the run has delivered?".
 * `search_docs` reaches the knowledge-base service over the network, but that
 * is a trusted internal component, so it is TOOL_EGRESS_NONE. `web_read`
 * fetches an arbitrary third-party URL, so it is TOOL_EGRESS_EXTERNAL: the URL
 * itself is an outbound channel.
 *
 * SCOPE -- AND A GAP THAT IS STILL OPEN
 *
 * This registry covers BUILT-IN tools only, which are the only population
 * enumerable at build time. Names it does not know return TOOL_EGRESS_UNSET.
 *
 * Callers currently handle UNSET in two ways: third-party MCP tools default to
 * denied (wfe_externalization.c), but every OTHER dynamic registration --
 * host-CLI tools especially -- still falls through to the legacy name lists and
 * can therefore resolve to permitted. That is the original fail-open hole,
 * surviving for one population. It is not closed here because denying all UNSET
 * at this layer would deny host-CLI Read/Edit/Grep and break gated runs; the
 * real fix is a class assigned at REGISTRATION time.
 *
 * So: this gate is fail-closed for built-ins and for third-party MCP, and it is
 * strictly stronger than the name lists it replaces -- but "fail-closed" is not
 * yet a property of the whole gate. See
 * docs/proposals/done/dynamic-tool-egress-classification.md.
 *
 * Pure: no engine, DB, or network dependencies, so it links everywhere the
 * workflow policy is consulted and unit-tests in isolation. */
#ifndef DEC_TOOL_EGRESS_H
#define DEC_TOOL_EGRESS_H 1

typedef enum
{
   /* Never valid for a registered tool. The zero value deliberately means
    * "undeclared" rather than "safe", so a forgotten declaration fails the
    * startup invariant instead of defaulting to ungated. */
   TOOL_EGRESS_UNSET = 0,

   /* No data leaves the trust boundary. Local work, or traffic to a trusted
    * internal service such as the knowledge-base sidecar. */
   TOOL_EGRESS_NONE = 1,

   /* Sends data to a destination outside the trust boundary. The payload need
    * not be a request body -- a fetched URL is itself an exfiltration channel. */
   TOOL_EGRESS_EXTERNAL = 2,

   /* Egress is command-dependent and cannot be decided from the tool name:
    * these tools run a caller-supplied command line. They are gated by shell
    * command INSPECTION (wfe_is_shell_tool plus command matching), not by this
    * classification. Declaring them COMMAND records that the decision is made
    * elsewhere, rather than falsely asserting they are safe. */
   TOOL_EGRESS_COMMAND = 3,
} tool_egress_t;

/* Declared class for `tool_name`, case-insensitively, resolving aliases to
 * their canonical tool. Returns TOOL_EGRESS_UNSET for any name not in the
 * built-in registry -- callers must treat UNSET as "not classified here" and
 * fall through to their own heuristics, NOT as "no egress". */
tool_egress_t tool_egress_for(const char *tool_name);

/* 1 when `tool_name` is a built-in declared TOOL_EGRESS_EXTERNAL. Returns 0 for
 * unknown names: this answers "is this a known externalizing built-in?", so a
 * caller that also handles unknown tools keeps its existing fallback. */
int tool_egress_is_external(const char *tool_name);

/* Registry enumeration, for the startup coverage invariant and its tests. */
int tool_egress_count(void);
const char *tool_egress_name_at(int index);      /* NULL when out of range */
tool_egress_t tool_egress_class_at(int index);   /* UNSET when out of range */
int tool_egress_is_alias_at(int index);          /* 1 when the entry is an alias */
const char *tool_egress_canonical_at(int index); /* alias target, NULL if canonical */

/* Case-insensitive name equality, exported so the startup invariant compares
 * declaration names the SAME way tool_egress_for() looks them up. Comparing
 * case-sensitively there would let "Grep" and "grep" both be declared while
 * lookup silently resolves only the first. */
int tool_egress_names_equal(const char *a, const char *b);

/* Human-readable class name, for diagnostics. Never NULL. */
const char *tool_egress_class_name(tool_egress_t cls);

#endif /* DEC_TOOL_EGRESS_H */
