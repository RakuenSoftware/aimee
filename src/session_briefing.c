/* session_briefing.c: Phase-2-Step-2 helpers that surface open
 * commitments and unresolved epistemic directives at session start.
 * See docs/proposals/done/personal-agent-phase-2-recall.md.
 *
 * The shared Go memory owner renders the persisted state.
 * Each helper returns a heap-allocated markdown fragment
 * (or NULL / "") so build_session_context can drop it in without
 * touching schema. */

#include "aimee.h"
#include "session_briefing.h"
#include "module_commands.h"
#include "json_fluent.h"
#include "cJSON.h"
#include <aimee/skills/skill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return NULL;
   cJSON_AddNumberToObject(args, "limit", limit);
   cJSON *response = NULL;
   cJSON_AddStringToObject(args, "operation", "prospective-briefing");
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", args, &response);
   if (rc <= 0 || !cJSON_IsObject(response) || strcmp(jo_cstr(response, "status"), "ok") != 0)
   {
      cJSON_Delete(response);
      response = NULL;
   }
   cJSON_Delete(args);
   const char *block =
       response ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "block")) : NULL;
   char *result = block && block[0] ? strdup(block) : NULL;
   cJSON_Delete(response);
   return result;
}

char *session_briefing_render_directives(int limit)
{
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return NULL;
   cJSON_AddNumberToObject(args, "limit", limit);
   cJSON *response = NULL;
   cJSON_AddStringToObject(args, "operation", "directive-briefing");
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", args, &response);
   if (rc <= 0 || !cJSON_IsObject(response) || strcmp(jo_cstr(response, "status"), "ok") != 0)
   {
      cJSON_Delete(response);
      response = NULL;
   }
   cJSON_Delete(args);
   const char *block =
       response ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "block")) : NULL;
   char *result = block && block[0] ? strdup(block) : NULL;
   cJSON_Delete(response);
   return result;
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
