/* guardrails_blast_radius.c: structural blast-radius advisory for edits (§7).
 * See guardrails_blast_radius.h for the safety contract (structural-only,
 * advisory, fail-open). */
#include "guardrails_blast_radius.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "kb_client.h" /* kb_client_index_list, kb_client_index_blast_radius */

int blast_radius_advisory_format(const blast_radius_t *br, const char *edited_path,
                                 int hub_threshold, char *msg_buf, size_t msg_len)
{
   if (!br || !msg_buf || msg_len == 0)
      return 0;
   int n = br->dependent_count;
   if (n < 0)
      n = 0;
   if (n > 64) /* dependents[] is fixed-size; never read past it */
      n = 64;
   if (n == 0)
      return 0; /* nothing structurally impacted — stay silent */

   const char *path = (edited_path && edited_path[0]) ? edited_path : "this file";

   /* Header: "ADVISORY: editing <path> has structural blast radius — N
    * dependent file(s) may be affected:" */
   int off = snprintf(msg_buf, msg_len,
                      "ADVISORY: editing %s has structural blast radius — %d dependent file%s "
                      "may be affected:",
                      path, n, n == 1 ? "" : "s");
   if (off < 0 || (size_t)off >= msg_len)
      return 1; /* header alone filled the buffer; still a valid advisory */

   int listed = n < BR_ADVISORY_MAX_NAMES ? n : BR_ADVISORY_MAX_NAMES;
   for (int i = 0; i < listed; i++)
   {
      const char *dep = br->dependents[i][0] ? br->dependents[i] : "(unknown)";
      int w =
          snprintf(msg_buf + off, msg_len - (size_t)off, " %s%s", dep, (i + 1 < listed) ? "," : "");
      if (w < 0 || (size_t)w >= msg_len - (size_t)off)
         return 1; /* ran out of room mid-list — truncated but valid */
      off += w;
   }
   if (n > listed)
   {
      int w = snprintf(msg_buf + off, msg_len - (size_t)off, " (+%d more)", n - listed);
      if (w < 0 || (size_t)w >= msg_len - (size_t)off)
         return 1;
      off += w;
   }

   /* High-centrality hub note (refactor-risk). */
   if (n >= hub_threshold)
      snprintf(msg_buf + off, msg_len - (size_t)off,
               " — high-centrality hub; review callers before changing its interface.");
   return 1;
}

int guardrails_blast_radius_for_abs_path(const char *abs_path, blast_radius_t *out)
{
   if (!abs_path || !abs_path[0] || !out)
      return -1;

   project_info_t projects[32];
   int pcount = kb_client_index_list(projects, 32);
   for (int p = 0; p < pcount; p++)
   {
      /* Match the project whose root is a path-boundary prefix of abs_path
       * (mirrors classify_path in guardrails.c). */
      size_t rlen = strlen(projects[p].root);
      if (strncmp(abs_path, projects[p].root, rlen) == 0 &&
          (abs_path[rlen] == '/' || abs_path[rlen] == '\0'))
      {
         const char *rel = abs_path + rlen;
         if (*rel == '/')
            rel++;
         memset(out, 0, sizeof(*out));
         if (kb_client_index_blast_radius(projects[p].name, rel, out) != 0)
            return -1; /* FAIL-OPEN: sidecar error -> no advisory */
         return 0;
      }
   }
   return -1; /* no indexed project owns this path */
}

void guardrails_blast_radius_advisory(const char *abs_path, char *msg_buf, size_t msg_len)
{
   /* Don't clobber a higher-priority guardrail message. */
   if (!msg_buf || msg_len == 0 || msg_buf[0] != '\0')
      return;
   if (!abs_path || !abs_path[0])
      return;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.guardrails_blast_radius_advisory_enabled)
      return;

   blast_radius_t br;
   if (guardrails_blast_radius_for_abs_path(abs_path, &br) != 0)
      return; /* FAIL-OPEN */
   blast_radius_advisory_format(&br, abs_path, BR_ADVISORY_HUB_THRESHOLD, msg_buf, msg_len);
}
