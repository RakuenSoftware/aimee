#include "delegate_role.h"

#include <string.h>

/* Known role aliases: map non-canonical names to the canonical role that
 * agents.json registers.  Extend this table as new planner roles appear. */
static const struct
{
   const char *alias;
   const char *canonical;
} g_role_aliases[] = {
    {"implement", "code"},        {"build", "code"},
    {"reviewer", "review"},       {"verifier", "validate"},
    {"test", "validate"},         {"check", "validate"},
    {"evaluate", "validate"},     {"evaluate-optimize", "validate"},
    {"inspect", "diagnose"},      {"research", "execute"},
    {"enforce", "execute"},       {"recall", "search"},
    {"synthesize", "summarize"},  {"rank-fuse", "reason"},
    {"classify-score", "reason"}, {"planner", "plan"},
    {"planning", "plan"},         {NULL, NULL},
};

static int delegate_agent_supports_role(const agent_t *agent, const char *role)
{
   if (!agent || !role || !role[0])
      return 0;
   for (int i = 0; i < agent->role_count; i++)
   {
      /* "all" is a wildcard: the agent serves every role. */
      if (strcmp(agent->roles[i], "all") == 0 || strcmp(agent->roles[i], role) == 0)
         return 1;
   }
   for (int i = 0; i < agent->exec_role_count; i++)
   {
      if (strcmp(agent->exec_roles[i], role) == 0)
         return 1;
   }
   return 0;
}

const char *delegate_role_canonicalize(const char *role)
{
   if (!role || !role[0])
      return role;
   for (int i = 0; g_role_aliases[i].alias; i++)
   {
      if (strcmp(role, g_role_aliases[i].alias) == 0)
         return g_role_aliases[i].canonical;
   }
   return role;
}

int delegate_role_is_write(const char *role)
{
   if (!role || !role[0])
      return 0;
   const char *canonical = delegate_role_canonicalize(role);
   return strcmp(canonical, "code") == 0 || strcmp(canonical, "refactor") == 0 ||
          /* Novel-mode write roles: they draft/edit manuscript files. */
          strcmp(canonical, "prose") == 0 || strcmp(canonical, "line-edit") == 0 ||
          /* Songwriter-mode write roles: they draft/edit lyric files. */
          strcmp(canonical, "lyric") == 0 || strcmp(canonical, "hook") == 0;
}

int delegate_role_enable_tools_by_default(const char *role)
{
   if (!role || !role[0])
      return 0;

   role = delegate_role_canonicalize(role);
   return strcmp(role, "review") == 0 || strcmp(role, "search") == 0 ||
          strcmp(role, "execute") == 0 || strcmp(role, "diagnose") == 0 ||
          strcmp(role, "validate") == 0 ||
          /* Novel-mode read-only roles inspect the world bible by default. */
          strcmp(role, "continuity") == 0 || strcmp(role, "beat-check") == 0 ||
          /* Songwriter-mode read-only roles inspect the songbook by default. */
          strcmp(role, "prosody") == 0 || strcmp(role, "songform") == 0;
}

int delegate_role_result_cache_enabled(const char *role)
{
   if (!role || !role[0])
      return 0;

   role = delegate_role_canonicalize(role);
   return strcmp(role, "summarize") == 0 || strcmp(role, "format") == 0 ||
          strcmp(role, "draft") == 0;
}

int delegate_role_auto_tools_for_invocation(const char *role, int max_turns, int explicit_tools)
{
   if (explicit_tools)
      return 1;
   if (max_turns == 1)
      return 0;
   return delegate_role_enable_tools_by_default(role);
}

void delegate_apply_max_turns_override(agent_config_t *cfg, int max_turns)
{
   if (!cfg || max_turns < 0)
      return;
   for (int i = 0; i < cfg->agent_count; i++)
      cfg->agents[i].max_turns = max_turns;
}

int delegate_default_max_turns_for_role(const char *role)
{
   if (!role || !role[0])
      return -1;

   role = delegate_role_canonicalize(role);
   if (strcmp(role, "review") == 0)
      return 20;
   if (strcmp(role, "validate") == 0 || strcmp(role, "search") == 0)
      return 12;
   if (strcmp(role, "diagnose") == 0)
      return 16;
   return -1;
}

int delegate_final_after_turns_for_role(const char *role)
{
   if (!role || !role[0])
      return -1;

   role = delegate_role_canonicalize(role);
   if (strcmp(role, "validate") == 0)
      return 8;
   if (strcmp(role, "search") == 0)
      return 10;
   if (strcmp(role, "diagnose") == 0)
      return 12;
   return -1;
}

void delegate_apply_max_turns_policy(agent_config_t *cfg, const char *role, int max_turns)
{
   if (!cfg)
      return;

   if (max_turns >= 0)
   {
      delegate_apply_max_turns_override(cfg, max_turns);
      return;
   }

   int default_cap = delegate_default_max_turns_for_role(role);
   if (default_cap <= 0)
      return;

   const char *canonical_role = delegate_role_canonicalize(role);
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (!delegate_agent_supports_role(&cfg->agents[i], canonical_role))
         continue;
      /* A per-agent declared cap (max_turns >= 0, where 0 == unlimited) is
       * honored verbatim; the role default is only a FLOOR for agents that
       * declared nothing (max_turns == -1 after config load). Never clamp a
       * declared cap down to the role default. */
      if (cfg->agents[i].max_turns < 0)
         cfg->agents[i].max_turns = default_cap;
   }
}
