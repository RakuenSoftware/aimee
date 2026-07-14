/* wfe_panel_roundtable.c: verified roundtable items -> per-lens wfe verdicts.
 * See the header for the contract. */
#include "wfe_panel_roundtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Is agent `name` one of the comma-separated tokens in `sources`? Token
 * compare, not substring: one agent's name may prefix another's. */
static int sources_contain(const char *sources, const char *name)
{
   if (!sources || !name || !name[0])
      return 0;
   size_t nl = strlen(name);
   const char *p = sources;
   while (*p)
   {
      while (*p == ' ' || *p == ',')
         p++;
      const char *end = strchr(p, ',');
      size_t tl = end ? (size_t)(end - p) : strlen(p);
      while (tl > 0 && (p[tl - 1] == ' '))
         tl--;
      if (tl == nl && strncmp(p, name, nl) == 0)
         return 1;
      if (!end)
         break;
      p = end + 1;
   }
   return 0;
}

/* Parse `location` as "<path>:<line>" (the panel's file:line convention).
 * Returns 1 and fills path/line on success; 0 for non-file:line locations
 * ("artifact section 3"), which have nothing to ground. */
static int location_parse(const char *location, char *path, size_t path_cap, int *line)
{
   if (!location || !location[0])
      return 0;
   const char *colon = strrchr(location, ':');
   if (!colon || colon == location || !colon[1])
      return 0;
   char *end = NULL;
   long n = strtol(colon + 1, &end, 10);
   if (!end || *end != '\0' || n < 1)
      return 0;
   size_t pl = (size_t)(colon - location);
   if (pl >= path_cap)
      return 0;
   memcpy(path, location, pl);
   path[pl] = '\0';
   *line = (int)n;
   return 1;
}

/* Does workdir/path exist with at least `line` lines? Repo-relative only. */
static int location_grounds(const char *workdir, const char *path, int line)
{
   if (!path[0] || path[0] == '/' || strstr(path, ".."))
      return 0;
   char abs[512];
   if (snprintf(abs, sizeof abs, "%s/%s", workdir, path) >= (int)sizeof abs)
      return 0;
   FILE *fp = fopen(abs, "r");
   if (!fp)
      return 0;
   int lines = 0, c, prev = '\n';
   while (lines < line && (c = fgetc(fp)) != EOF)
   {
      if (c == '\n')
         lines++;
      prev = c;
   }
   if (prev != '\n')
      lines++; /* a final line without a trailing newline still counts */
   fclose(fp);
   return lines >= line;
}

/* Is this surviving item blocking FOR THE GATE? Severity must say blocking,
 * and a file:line-shaped location must additionally ground in the worktree. */
static int item_blocks(const roundtable_review_item_t *it, const char *workdir)
{
   if (strcmp(it->severity, "blocking") != 0)
      return 0;
   char path[256];
   int line = 0;
   if (workdir && workdir[0] && location_parse(it->location, path, sizeof path, &line))
      return location_grounds(workdir, path, line);
   return 1; /* non-file:line location: the evidence replay already vetted it */
}

static void feedback_append_item(wfe_verdict_t *v, const roundtable_review_item_t *it, int blocks)
{
   size_t used = strlen(v->feedback);
   if (used >= sizeof v->feedback - 1)
      return;
   snprintf(v->feedback + used, sizeof v->feedback - used, "%s- [%s] %s: %s%s%s", used ? "\n" : "",
            blocks ? "blocking" : it->severity, it->location[0] ? it->location : "(general)",
            it->summary, it->recommendation[0] ? " -> " : "", it->recommendation);
}

int wfe_panel_verdicts_from_roundtable(const roundtable_result_t *rt, const char *const *lens,
                                       const char *const *seat_agent, int nlens,
                                       const char *artifact_hash, const char *workdir,
                                       wfe_verdict_t *out)
{
   if (!rt || !lens || !seat_agent || !out || nlens <= 0)
      return -1;

   for (int i = 0; i < nlens; i++)
   {
      memset(&out[i], 0, sizeof out[i]);
      snprintf(out[i].persona, sizeof out[i].persona, "%s", lens[i] ? lens[i] : "");
      snprintf(out[i].model, sizeof out[i].model, "%s", seat_agent[i] ? seat_agent[i] : "");
      out[i].schema_version = WFE_VERDICT_SCHEMA;
      snprintf(out[i].reviewed_content_hash, sizeof out[i].reviewed_content_hash, "%s",
               artifact_hash ? artifact_hash : "");
      out[i].kind = WFE_V_APPROVE; /* refined below by the items */
   }

   for (int k = 0; k < rt->item_count; k++)
   {
      const roundtable_review_item_t *it = &rt->items[k];
      int blocks = item_blocks(it, workdir);
      int attributed = 0;
      for (int i = 0; i < nlens; i++)
      {
         if (!sources_contain(it->sources, seat_agent[i]))
            continue;
         attributed = 1;
         feedback_append_item(&out[i], it, blocks);
         if (blocks)
         {
            out[i].kind = WFE_V_REQUEST_CHANGES;
            out[i].high_sev_blockers++;
         }
      }
      /* A blocking item nobody on the panel is credited with must still loop
       * the gate (fail closed), not silently pass: pin it on lens 0. */
      if (blocks && !attributed)
      {
         feedback_append_item(&out[0], it, blocks);
         out[0].kind = WFE_V_REQUEST_CHANGES;
         out[0].high_sev_blockers++;
      }
   }
   return nlens;
}
