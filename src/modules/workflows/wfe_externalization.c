/* wfe_externalization.c -- pure externalization denylist + run-state guard.
 * See wfe_externalization.h. No engine/DB deps so it is unit-testable in
 * isolation and links everywhere the workflow policy is consulted. */
#include "wfe_externalization.h"

#include <ctype.h>
#include <string.h>

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

/* Exact/canonical externalization tool names. Extend only with review. */
static const char *DENY_EXACT[] = {
    "pr.open",      "pr_open",  "propose_pr",   "open_pr", "merge",
    "pr.merge",     "git_push", "git-push",     "push",    "gh_pr",
    "web_fetch",    "webfetch", "fetch",        "http",    "http_request",
    "curl",         "wget",     "send_email",   "email",   "notify",
    "notification", "slack",    "post_comment", "comment", NULL};

/* Substrings that mark an externalizing action regardless of surrounding name
 * (e.g. an MCP tool `mcp__github__create_pull_request`). Lowercase. */
static const char *DENY_SUBSTR[] = {
    "push",   "create_pull", "pull_request", "merge",   "publish", "deploy", "release",
    "upload", "egress",      "outbound",     "webhook", "notify",  NULL};

int wfe_is_externalization_tool(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;
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
