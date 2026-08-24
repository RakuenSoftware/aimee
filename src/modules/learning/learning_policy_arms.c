/* learning_policy_arms.c: instructions as measurable arms (S6).
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/policy_arms.h>

#include <aimee/learning/learning.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   const char *point;
   const char *arms[LEARNING_POLICY_MAX_ARMS];
   const char *shipped_default;
   /* Promotion writes here; empty means "use shipped_default". Process-local
    * on purpose: a promotion is an operator decision about this installation,
    * and persisting it is a separate contract with its own audit. */
   char promoted[LEARNING_POLICY_ARM_LEN];
} policy_point_t;

static policy_point_t g_points[] = {
    {LEARNING_POLICY_PLAN_ADVISORY,
     {LEARNING_POLICY_ADVISORY_OFF, LEARNING_POLICY_ADVISORY_BRIEF, LEARNING_POLICY_ADVISORY_FULL,
      NULL},
     /* The block S3 already renders. Shipping "full" as the default means this
      * registry changes nothing until someone deliberately samples or
      * promotes — the arms exist to make the current behaviour answerable, not
      * to alter it. */
     LEARNING_POLICY_ADVISORY_FULL,
     ""},
};

static learning_policy_sampler_fn g_sampler;
static learning_policy_reward_fn g_reward_sink;

void learning_policy_register_sampler(learning_policy_sampler_fn sampler)
{
   g_sampler = sampler;
}

void learning_policy_register_reward_sink(learning_policy_reward_fn sink)
{
   g_reward_sink = sink;
}

static policy_point_t *find_point(const char *decision_point)
{
   if (!decision_point || !decision_point[0])
      return NULL;
   for (size_t i = 0; i < sizeof(g_points) / sizeof(g_points[0]); i++)
      if (strcmp(g_points[i].point, decision_point) == 0)
         return &g_points[i];
   return NULL;
}

static int point_arm_count(const policy_point_t *p)
{
   int n = 0;
   while (n < LEARNING_POLICY_MAX_ARMS && p->arms[n])
      n++;
   return n;
}

int learning_policy_arms(const char *decision_point, char (*out)[LEARNING_POLICY_ARM_LEN], int max)
{
   policy_point_t *p = find_point(decision_point);
   if (!p || !out || max <= 0)
      return -1;
   int n = point_arm_count(p);
   if (n > max)
      n = max;
   for (int i = 0; i < n; i++)
      snprintf(out[i], LEARNING_POLICY_ARM_LEN, "%s", p->arms[i]);
   return n;
}

int learning_policy_arm_is_valid(const char *decision_point, const char *arm)
{
   policy_point_t *p = find_point(decision_point);
   if (!p || !arm || !arm[0])
      return 0;
   for (int i = 0; i < LEARNING_POLICY_MAX_ARMS && p->arms[i]; i++)
      if (strcmp(p->arms[i], arm) == 0)
         return 1;
   return 0;
}

const char *learning_policy_default_arm(const char *decision_point)
{
   policy_point_t *p = find_point(decision_point);
   if (!p)
      return NULL;
   return p->promoted[0] ? p->promoted : p->shipped_default;
}

int learning_policy_select(const char *decision_point, char *out, size_t out_len)
{
   policy_point_t *p = find_point(decision_point);
   if (!p || !out || out_len == 0)
      return -1;

   const char *chosen = learning_policy_default_arm(decision_point);

   if (g_sampler)
   {
      char arms[LEARNING_POLICY_MAX_ARMS][LEARNING_POLICY_ARM_LEN];
      int n = learning_policy_arms(decision_point, arms, LEARNING_POLICY_MAX_ARMS);
      if (n > 0)
      {
         int idx = g_sampler(decision_point, arms, n);
         /* An out-of-range answer is treated as declining, not as an error:
          * the sampler lives in another process and a bad one must never be
          * able to inject a fragment this point does not declare. */
         if (idx >= 0 && idx < n)
            chosen = arms[idx];
      }
   }

   snprintf(out, out_len, "%s", chosen ? chosen : "");
   return 0;
}

int learning_policy_report(const char *decision_point, const char *arm, double reward)
{
   if (!learning_policy_arm_is_valid(decision_point, arm))
      return -1; /* crediting an arm nobody could have chosen is worse than losing the sample */
   if (!g_reward_sink)
      return -1;
   g_reward_sink(decision_point, arm, reward);
   return 0;
}

int learning_policy_promote(const char *decision_point, const char *arm)
{
   policy_point_t *p = find_point(decision_point);
   if (!p || !learning_policy_arm_is_valid(decision_point, arm))
      return -1;

   /* The instructions are what the system will judge itself by next. A loop
    * whose evidence has gone self-referential does not get to rewrite them. */
   if (learning_gate_check(NULL) != LEARNING_GATE_OPEN)
      return -1;

   snprintf(p->promoted, sizeof(p->promoted), "%s", arm);
   return 0;
}

void learning_policy_reset(void)
{
   for (size_t i = 0; i < sizeof(g_points) / sizeof(g_points[0]); i++)
      g_points[i].promoted[0] = '\0';
   g_sampler = NULL;
   g_reward_sink = NULL;
}
