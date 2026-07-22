/* wfe_enforce.c -- pure S2 enforcement policy cores. See wfe_enforce.h. */
#include "wfe_enforce.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wfe_externalization.h" /* wfe_is_deliver_primitive */

static int eq_ci(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   for (; *a && *b; a++, b++)
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
         return 0;
   return *a == *b;
}

static int has_ci(const char *hay, const char *needle) /* needle lowercase */
{
   if (!hay || !needle)
      return 0;
   size_t nl = strlen(needle);
   if (!nl)
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

wfe_tool_surface_t wfe_block_default_surface(wfe_block_type_t t)
{
   switch (t)
   {
   /* the primary reads / scopes / reviews / gates; delegates do the writing. */
   case WFE_BLK_UNDERSTAND:
   case WFE_BLK_SPLIT:
   case WFE_BLK_REVIEW:
   case WFE_BLK_FREEZE:
   case WFE_BLK_GATE_ROUNDTABLE:
   case WFE_BLK_GATE_HUMAN:
   case WFE_BLK_GATE_CI:
   case WFE_BLK_CHECK_MERGEABLE:
   case WFE_BLK_GATE_DELIVER:
      return WFE_SURFACE_READONLY;
   /* the primary launches delegates; it does not write the tree directly. */
   case WFE_BLK_IMPLEMENT:
   case WFE_BLK_DOCUMENT:
      return WFE_SURFACE_DELEGATE;
   /* author.proposal / pr.open / merge / custom / unknown: not part of the
    * interactive manager loop -> unrestricted (the deliver gate still applies). */
   default:
      return WFE_SURFACE_FULL;
   }
}

int wfe_is_write_tool(const char *t)
{
   if (!t || !t[0])
      return 0;
   static const char *W[] = {"Edit",        "Write",       "NotebookEdit", "MultiEdit",
                             "str_replace", "apply_patch", "patch",        NULL};
   for (int i = 0; W[i]; i++)
      if (eq_ci(t, W[i]))
         return 1;
   if (has_ci(t, "write_file") || has_ci(t, "edit_file") || has_ci(t, "apply_patch") ||
       has_ci(t, "create_file"))
      return 1;
   return 0;
}

int wfe_is_delegate_tool(const char *t)
{
   if (!t || !t[0])
      return 0;
   static const char *D[] = {"delegate", "Task", "Agent", "spawn_agent", "Subagent", NULL};
   for (int i = 0; D[i]; i++)
      if (eq_ci(t, D[i]))
         return 1;
   if (has_ci(t, "delegate") || has_ci(t, "subagent"))
      return 1;
   return 0;
}

int wfe_surface_allows(wfe_tool_surface_t surface, const char *tool_name, int delivered)
{
   if (!tool_name || !tool_name[0])
      return 0; /* fail closed on an unknown tool */
   /* deliver primitives are gated on delivery at every surface. */
   if (!delivered && wfe_is_deliver_primitive(tool_name))
      return 0;
   switch (surface)
   {
   case WFE_SURFACE_FULL:
      return 1; /* delivery already gated above */
   case WFE_SURFACE_DELEGATE:
      return wfe_is_write_tool(tool_name) ? 0 : 1; /* no direct write; delegate ok */
   case WFE_SURFACE_READONLY:
   default:
      return (wfe_is_write_tool(tool_name) || wfe_is_delegate_tool(tool_name)) ? 0 : 1;
   }
}

wfe_fail_action_t wfe_enforce_fail_action(wfe_fail_class_t cls, int hard, int is_deliver_or_write)
{
   if (cls == WFE_FAIL_POLICY)
      return WFE_ACT_FAIL_CLOSED; /* a denial is not an error -> always fail closed */
   /* instrumentation failure: preserve chat, but never let a lookup error bypass
    * delivery/write enforcement when the dial is hard. */
   if (hard && is_deliver_or_write)
      return WFE_ACT_FAIL_CLOSED;
   return WFE_ACT_FAIL_OPEN_CHAT;
}

wfe_enforce_stage_t wfe_enforce_stage_parse(const char *s)
{
   if (!s || !s[0])
      return WFE_ENFORCE_OFF;
   if (eq_ci(s, "advisory"))
      return WFE_ENFORCE_ADVISORY;
   if (eq_ci(s, "soft"))
      return WFE_ENFORCE_SOFT;
   if (eq_ci(s, "hard"))
      return WFE_ENFORCE_HARD;
   return WFE_ENFORCE_OFF;
}

const char *wfe_enforce_stage_name(wfe_enforce_stage_t s)
{
   switch (s)
   {
   case WFE_ENFORCE_ADVISORY:
      return "advisory";
   case WFE_ENFORCE_SOFT:
      return "soft";
   case WFE_ENFORCE_HARD:
      return "hard";
   case WFE_ENFORCE_OFF:
   default:
      return "off";
   }
}

int wfe_enforce_stage_restricts(wfe_enforce_stage_t s)
{
   return s == WFE_ENFORCE_SOFT || s == WFE_ENFORCE_HARD;
}

int wfe_enforce_stage_refuses(wfe_enforce_stage_t s)
{
   return s == WFE_ENFORCE_HARD;
}

int wfe_advance_cas_ok(const char *observed_stage, const char *actual_stage)
{
   if (!observed_stage || !observed_stage[0] || !actual_stage || !actual_stage[0])
      return -1;
   return strcmp(observed_stage, actual_stage) == 0 ? 1 : 0;
}

/* id-charset guard so an unexpected value can never break the JSON/user text or
 * inject content -- ids are validated elsewhere, this is defense in depth. */
static int safe_id(const char *s)
{
   if (!s || !s[0])
      return 0;
   for (const char *p = s; *p; p++)
   {
      char c = *p;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.'))
         return 0;
   }
   return 1;
}

void wfe_enforce_user_message(wfe_enforce_stage_t stage, const char *gate, const char *work_item_id,
                              char *buf, size_t n)
{
   if (!buf || !n)
      return;
   const char *g = safe_id(gate) ? gate : "the workflow gate";
   const char *verb = wfe_enforce_stage_refuses(stage) ? "was blocked" : "is being held";
   if (safe_id(work_item_id))
      snprintf(buf, n, "This change %s: it must pass %s before it can be delivered (work item %s).",
               verb, g, work_item_id);
   else
      snprintf(buf, n, "This change %s: it must pass %s before it can be delivered.", verb, g);
}

int wfe_lease_ttl_secs(void)
{
   const char *v = getenv("AIMEE_WORKFLOW_LEASE_TTL_SECS");
   if (v && v[0])
   {
      char *end = NULL;
      long n = strtol(v, &end, 10);
      if (end && *end == '\0' && n >= 0 && n <= 604800) /* clamp to <= 7 days */
         return (int)n;
   }
   return 3600; /* default: 1h idle-to-stale */
}
