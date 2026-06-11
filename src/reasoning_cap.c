/* reasoning_cap.c: deterministic complexity score -> reasoning-effort cap (§5).
 * Pure and dependency-free so it is unit-tested directly. See reasoning_cap.h. */
#include "reasoning_cap.h"
#include <string.h>

int reasoning_complexity_score(int message_count, size_t content_len, int has_tools)
{
   int score = 0;

   /* Length is the dominant signal: a longer turn generally needs more
    * reasoning. Bucketed so the score stays deterministic and stable. */
   if (content_len >= 6000)
      score += 7;
   else if (content_len >= 2000)
      score += 5;
   else if (content_len >= 800)
      score += 3;
   else if (content_len >= 200)
      score += 1;

   /* A multi-message conversation is more complex than a single prompt. */
   if (message_count > 8)
      score += 3;
   else if (message_count > 3)
      score += 2;
   else if (message_count > 1)
      score += 1;

   /* Tool-bearing turns can fan out into multi-step work. */
   if (has_tools)
      score += 2;

   if (score < 0)
      score = 0;
   if (score > 10)
      score = 10;
   return score;
}

/* Ordinal rank of a reasoning-effort enum; 0 means unknown/empty. */
static int effort_rank(const char *e)
{
   if (!e || !e[0])
      return 0;
   if (strcmp(e, "low") == 0)
      return 1;
   if (strcmp(e, "medium") == 0)
      return 2;
   if (strcmp(e, "high") == 0)
      return 3;
   if (strcmp(e, "xhigh") == 0)
      return 4;
   if (strcmp(e, "max") == 0)
      return 5;
   return 0;
}

const char *reasoning_effort_capped(const char *configured_effort, int score)
{
   int cfg_rank = effort_rank(configured_effort);
   if (cfg_rank == 0)
      return configured_effort; /* empty/unknown -> never impose a cap */

   /* Ceiling implied by the complexity score. */
   const char *ceiling_str;
   int ceiling_rank;
   if (score <= 2)
   {
      ceiling_str = "low";
      ceiling_rank = 1;
   }
   else if (score <= 5)
   {
      ceiling_str = "medium";
      ceiling_rank = 2;
   }
   else if (score <= 8)
   {
      ceiling_str = "high";
      ceiling_rank = 3;
   }
   else
   {
      return configured_effort; /* high complexity -> no cap */
   }

   /* Only ever lower: keep the configured effort if it is already at or below
    * the ceiling. */
   if (cfg_rank <= ceiling_rank)
      return configured_effort;
   return ceiling_str;
}
