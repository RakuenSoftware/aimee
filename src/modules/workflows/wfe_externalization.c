/* wfe_externalization.c -- pure externalization denylist + run-state guard.
 * See wfe_externalization.h. No engine/DB deps so it is unit-testable in
 * isolation and links everywhere the workflow policy is consulted. */
#include "wfe_externalization.h"

#include "tool_egress.h"

#include <ctype.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

/* case-insensitive substring match (needle assumed lowercase). */
static int has_ci(const char *hay, const char *needle)
{
   if (!hay || !needle)
      return 0;
   size_t nl = strlen(needle);
   if (nl == 0)
      return 0;
   for (const char *p = hay; *p; p++)
   {
      size_t i = 0;
      while (i < nl && p[i] && (char)tolower((unsigned char)p[i]) == needle[i])
         i++;
      if (i == nl)
         return 1;
   }
   return 0;
}

/* case-insensitive equality against a canonical tool name. */
static int eq_ci(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   for (; *a && *b; a++, b++)
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
         return 0;
   return *a == *b;
}

/* Exact/canonical externalization tool names.
 *
 * NON-AUTHORITATIVE. It is a backstop, never the reason a tool is safe:
 *   - built-ins are classified by declaration in tool_egress.c, kept in sync
 *     with the real tool table by a startup invariant;
 *   - third-party MCP tools default to external above, so this list is not what
 *     catches them either.
 * What is left for this list is host-CLI names and generic spellings (`curl`,
 * `wget`, `http_request`, `slack`) that no built-in registers.
 *
 * IT IS STILL THE ONLY CONTROL FOR THAT POPULATION, AND IT IS FAIL-OPEN THERE.
 * An unknown non-MCP dynamic tool that matches no entry below is PERMITTED. So
 * a missing entry here can still leave such a tool ungated -- the declaration
 * registry removed that risk for built-ins and the MCP default removed it for
 * third-party MCP servers, but neither covers host-CLI or other dynamic
 * registrations. Closing it needs registration-time classification; see
 * docs/proposals/done/dynamic-tool-egress-classification.md.
 *
 * Extend only with review. */
static const char *DENY_EXACT[] = {
    "pr.open",   "pr_open",  "propose_pr",   "open_pr", "merge",
    "pr.merge",  "git_push", "git-push",     "push",    "gh_pr",
    "web_fetch", "webfetch", "fetch",        "http",    "http_request",
    "web_read",  "webread",  "curl",         "wget",    "send_email",
    "email",     "notify",   "notification", "slack",   "post_comment",
    "comment",   NULL};

/* Substrings that mark an externalizing action regardless of surrounding name
 * (e.g. an MCP tool `mcp__github__create_pull_request`). Lowercase. */
static const char *DENY_SUBSTR[] = {
    "push",   "create_pull", "pull_request", "merge",   "publish", "deploy", "release",
    "upload", "egress",      "outbound",     "webhook", "notify",  NULL};

/* A tool served by a THIRD-PARTY MCP server.
 *
 * Built-ins are covered by the declaration registry, but MCP tools are
 * registered dynamically at runtime, so no startup invariant can enumerate
 * them. Falling through to the name lists for these would preserve exactly the
 * fail-open hole this module exists to close: an MCP tool whose name happens to
 * miss every deny substring would be treated as in-boundary.
 *
 * An MCP call crosses a process boundary and usually a network one, to a server
 * this gate cannot inspect. So the default is EXTERNAL, and over-blocking is
 * the intended direction for a security gate.
 *
 * aimee's own MCP server (`mcp__aimee__*`) is excluded: it is the same trust
 * domain, and treating it as external would deny in-process delegation during
 * gated runs. */
static int is_third_party_mcp_tool(const char *tool_name)
{
   static const char *const MCP_PREFIX = "mcp__";
   if (strncasecmp(tool_name, MCP_PREFIX, strlen(MCP_PREFIX)) != 0)
      return 0;
   /* The own-server exemption must match the server name EXACTLY, at its
    * delimiter. A bare prefix test on "mcp__aimee" would also exempt
    * `mcp__aimeeevil__exfiltrate`, letting a third-party server opt itself out
    * of the gate just by choosing a name that starts with ours. */
   if (strcasecmp(tool_name, "mcp__aimee") == 0)
      return 0;
   if (strncasecmp(tool_name, "mcp__aimee__", strlen("mcp__aimee__")) == 0)
      return 0;
   return 1;
}

int wfe_is_externalization_tool(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;
   /* Declaration first: for a built-in tool this is the authoritative answer,
    * and it cannot be forgotten -- a built-in with no declaration fails the
    * startup invariant rather than falling through to the name lists below. */
   if (tool_egress_is_external(tool_name))
      return 1;
   /* Dynamically registered third-party tools cannot be covered by a startup
    * invariant, so they are denied by default rather than by name matching. */
   if (is_third_party_mcp_tool(tool_name))
      return 1;
   for (int i = 0; DENY_EXACT[i]; i++)
      if (eq_ci(tool_name, DENY_EXACT[i]))
         return 1;
   for (int i = 0; DENY_SUBSTR[i]; i++)
      if (has_ci(tool_name, DENY_SUBSTR[i]))
         return 1;
   return 0;
}

int wfe_externalization_tool_permitted(const char *tool_name, int delivered)
{
   if (delivered)
      return 1; /* post-delivery: the guard has lifted */
   if (!tool_name)
      return 0; /* fail closed on an unknown tool pre-delivery */
   return wfe_is_externalization_tool(tool_name) ? 0 : 1;
}

/* Closed DELIVER_PRIMITIVES set: actions that mark work delivered/accepted or
 * make it visible outside the gated run. Extend only with review. */
static const char *DELIVER_EXACT[] = {
    "pr.open", "pr_open", "open_pr",     "propose_pr",  "pr.merge", "merge",
    "gh_pr",   "accept",  "mark_done",   "mark-done",   "deploy",   "publish",
    "release", "tag",     "close_issue", "issue_close", NULL};
static const char *DELIVER_SUBSTR[] = {"create_pull", "pull_request", "merge", "publish",
                                       "deploy",      "release",      NULL};

int wfe_is_deliver_primitive(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;
   for (int i = 0; DELIVER_EXACT[i]; i++)
      if (eq_ci(tool_name, DELIVER_EXACT[i]))
         return 1;
   for (int i = 0; DELIVER_SUBSTR[i]; i++)
      if (has_ci(tool_name, DELIVER_SUBSTR[i]))
         return 1;
   return 0;
}
