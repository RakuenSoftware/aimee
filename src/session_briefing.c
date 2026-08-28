/* session_briefing.c: Phase-2-Step-2 helpers that surface open
 * commitments and unresolved epistemic directives at session start.
 * See docs/proposals/done/personal-agent-phase-2-recall.md.
 *
 * These build on state already persisted by the phase-1 primitives
 * (memory_prospective_t, memory_directive_t); they don't introduce a
 * new store. Each helper returns a heap-allocated markdown fragment
 * (or NULL / "") so build_session_context can drop it in without
 * touching schema. */

#include "aimee.h"
#include "memory.h"
#include "session_briefing.h"
#include <aimee/skills/skill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default caps for the two briefing sections. Both are conservative
 * so a pathologically large backlog never blows out the session-start
 * output. Operators who want more can ask via the dedicated commands
 * (aimee memory reminders list, aimee memory directives list). */
#ifndef SESSION_BRIEFING_COMMITMENTS_DEFAULT_LIMIT
#define SESSION_BRIEFING_COMMITMENTS_DEFAULT_LIMIT 8
#endif
#ifndef SESSION_BRIEFING_DIRECTIVES_DEFAULT_LIMIT
#define SESSION_BRIEFING_DIRECTIVES_DEFAULT_LIMIT 5
#endif
#ifndef SESSION_BRIEFING_SKILLS_DEFAULT_LIMIT
#define SESSION_BRIEFING_SKILLS_DEFAULT_LIMIT 24
#endif

/* Small heap-growing buffer so the formatters don't have to guess
 * capacity up front. Keeps allocations local; session-start context
 * assembly pays the copy once. */
typedef struct
{
   char *buf;
   size_t len;
   size_t cap;
} sb_str_t;

static int sb_reserve(sb_str_t *s, size_t need)
{
   if (s->len + need + 1 <= s->cap)
      return 0;
   size_t cap = s->cap ? s->cap : 256;
   while (cap < s->len + need + 1)
      cap *= 2;
   char *nb = realloc(s->buf, cap);
   if (!nb)
      return -1;
   s->buf = nb;
   s->cap = cap;
   return 0;
}

static int sb_appendf(sb_str_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int sb_appendf(sb_str_t *s, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   va_list ap2;
   va_copy(ap2, ap);
   int n = vsnprintf(NULL, 0, fmt, ap);
   va_end(ap);
   if (n < 0)
   {
      va_end(ap2);
      return -1;
   }
   if (sb_reserve(s, (size_t)n) != 0)
   {
      va_end(ap2);
      return -1;
   }
   vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap2);
   va_end(ap2);
   s->len += (size_t)n;
   return 0;
}

char *session_briefing_render_commitments(int limit)
{
   if (limit <= 0)
      limit = SESSION_BRIEFING_COMMITMENTS_DEFAULT_LIMIT;
   if (limit > 32)
      limit = 32;

   memory_prospective_t rows[32];
   int n = memory_prospective_list(MEMORY_PROSPECTIVE_STATE_ARMED, rows, limit);
   if (n <= 0)
      return NULL;

   sb_str_t s = {0};
   if (sb_appendf(&s, "# Open Commitments\n") != 0)
   {
      free(s.buf);
      return NULL;
   }
   for (int i = 0; i < n; i++)
   {
      const char *trig = rows[i].trigger_text[0] ? rows[i].trigger_text : "(no trigger)";
      const char *act = rows[i].action_text[0] ? rows[i].action_text : "(no action)";
      if (rows[i].valid_until[0])
      {
         if (sb_appendf(&s, "- when `%.120s` → %.200s  [until %s]\n", trig, act,
                        rows[i].valid_until) != 0)
            break;
      }
      else
      {
         if (sb_appendf(&s, "- when `%.120s` → %.200s\n", trig, act) != 0)
            break;
      }
   }
   if (sb_appendf(&s, "\n") != 0)
   {
      free(s.buf);
      return NULL;
   }
   return s.buf;
}

char *session_briefing_render_directives(int limit)
{
   if (limit <= 0)
      limit = SESSION_BRIEFING_DIRECTIVES_DEFAULT_LIMIT;
   if (limit > 32)
      limit = 32;

   /* memory_directive_list orders by priority DESC, created DESC — top
    * of the list is the most urgent open question. */
   memory_directive_t rows[32];
   int n = memory_directive_list("open", NULL, rows, limit);
   if (n <= 0)
      return NULL;

   sb_str_t s = {0};
   if (sb_appendf(&s, "# Open Questions\n") != 0)
   {
      free(s.buf);
      return NULL;
   }
   for (int i = 0; i < n; i++)
   {
      const char *q = rows[i].question[0] ? rows[i].question : "(unnamed)";
      const char *cause = rows[i].cause[0] ? rows[i].cause : "";
      if (cause[0])
      {
         if (sb_appendf(&s, "- [p%d · %s] %.300s\n", rows[i].priority, cause, q) != 0)
            break;
      }
      else
      {
         if (sb_appendf(&s, "- [p%d] %.300s\n", rows[i].priority, q) != 0)
            break;
      }
   }
   if (sb_appendf(&s, "\n") != 0)
   {
      free(s.buf);
      return NULL;
   }
   return s.buf;
}

char *session_briefing_render_skill_index(const char *project_root, int limit)
{
   if (limit <= 0)
      limit = SESSION_BRIEFING_SKILLS_DEFAULT_LIMIT;
   if (limit > SKILL_MAX_SKILLS)
      limit = SKILL_MAX_SKILLS;

   char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
   /* List the full bounded catalog, then apply the display limit only after a
    * skill has passed load/approval checks. Otherwise an unapproved project
    * skill at the front of the precedence order can consume the limit and hide
    * trusted bundled skills from the session prompt. */
   int n = skill_list(project_root, names, SKILL_MAX_SKILLS);
   if (n <= 0)
      return NULL;

   sb_str_t s = {0};
   if (sb_appendf(&s, "# Skill Dispatch\n"
                      "Before responding or acting, check the skill index. If a skill matches "
                      "the task, activate it with `/skill <name>`, announce which skill applies "
                      "and why, then follow the body.\n\n"
                      "## Available Skills\n") != 0)
   {
      free(s.buf);
      return NULL;
   }

   int rendered = 0;
   for (int i = 0; i < n; i++)
   {
      if (rendered >= limit)
         break;
      char desc[512] = "";
      if (skill_description(project_root, names[i], desc, sizeof(desc)) != 0 || !desc[0])
         continue;
      if (sb_appendf(&s, "- %s: %.500s\n", names[i], desc) != 0)
         break;
      rendered++;
   }

   if (rendered == 0)
   {
      free(s.buf);
      return NULL;
   }
   if (sb_appendf(&s, "\n") != 0)
   {
      free(s.buf);
      return NULL;
   }
   return s.buf;
}
