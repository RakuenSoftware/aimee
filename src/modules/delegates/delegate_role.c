#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/module_api.h>
#include "role_templates.h" /* role_template_max_turns (per-role cap) */

#include <string.h>

static delegate_role_canonicalizer_fn g_canonicalizer;

void delegate_role_register_canonicalizer(delegate_role_canonicalizer_fn canonicalizer)
{
   g_canonicalizer = canonicalizer;
}

static int delegate_agent_supports_role(const agent_t *agent, const char *role)
{
   if (!agent || !role || !role[0])
      return 0;
   /* Single source of truth for role eligibility: has the `all` wildcard or the
    * role itself. No exec-role fallback (see agent_has_role / agent_supports_role). */
   return agent_has_role(agent, role);
}

/* Roles deleted in the persona-vs-role cull. They are named here so an operator
 * or custom persona that still requests one gets a CLEAR error, rather than the
 * hazardous half-supported state of the name still being accepted as an
 * arbitrary role string while its semantics (write classification, tool
 * defaults, built-in template) have all been removed - which would silently hand
 * back a read-only delegate with a generic prompt. */
static const char *const g_removed_roles[] = {"prose",   "line-edit", "lyric", "hook",
                                              "prosody", "songform",  NULL};

const char *delegate_role_removed_reason(const char *role)
{
   if (!role || !role[0])
      return NULL;
   for (int i = 0; g_removed_roles[i]; i++)
   {
      if (strcmp(role, g_removed_roles[i]) == 0)
         /* Do NOT advertise "declare it in exec_roles" as a workaround: the CLI
          * rejects the name before routing is consulted, so that advice would
          * send an operator down a path that cannot work. */
         return "removed: novel/songwriter work is a PERSONA concern, not a delegate role. "
                "Use a persona with a general role such as draft, review or validate.";
   }
   return NULL;
}

static delegate_role_policy_fn g_role_policy;

void delegate_register_role_policy_provider(delegate_role_policy_fn provider)
{
   g_role_policy = provider;
}

/* Fails closed to `fallback`: with no answer, claim nothing.
 *
 * `c` carries a fact the module cannot look up. Only the auto-tools op reads it
 * today, and that op is the only one that does NOT require a role: it asks about
 * a permission the caller already resolved, so an empty role is a legitimate
 * question rather than a missing one. */
static int role_policy_ask_ex(int op, const char *role, int a, int b, int c, int fallback)
{
   int out = fallback;
   if (!g_role_policy)
      return fallback;
   if (g_role_policy(op, role ? role : "", a, b, c, &out) != 0)
      return fallback;
   return out;
}

/* The role-keyed ops. No role is no answer: every one of these IS a fact about
 * the role, so there is nothing to say about a delegate that has none. */
static int role_policy_ask(int op, const char *role, int a, int b, int fallback)
{
   if (!role || !role[0])
      return fallback;
   return role_policy_ask_ex(op, role, a, b, 0, fallback);
}

int delegate_role_is_write(const char *role)
{
   return role_policy_ask(DELEGATE_ROLE_OP_IS_WRITE, role, 0, 0, 0);
}

int delegate_role_result_cache_enabled(const char *role)
{
   return role_policy_ask(DELEGATE_ROLE_OP_CACHE, role, 0, 0, 0);
}

int delegate_role_needs_parent_diff(const char *role)
{
   return role_policy_ask(DELEGATE_ROLE_OP_PARENT_DIFF, role, 0, 0, 0);
}

int delegate_role_task_shape(const char *role)
{
   return role_policy_ask(DELEGATE_ROLE_OP_TASK_SHAPE, role, 0, 0, 0);
}

int delegate_auto_tools_for_invocation(int holds_tools, int max_turns, int explicit_tools)
{
   if (explicit_tools)
      return 1;
   /* The role is not consulted: the caller resolved the permission, and that
      answer already accounts for a role an operator defined. */
   return role_policy_ask_ex(DELEGATE_ROLE_OP_AUTO_TOOLS, "", max_turns, explicit_tools,
                             holds_tools, 0);
}

int delegate_final_after_turns_for_role(const char *role)
{
   return role_policy_ask(DELEGATE_ROLE_OP_FINAL_TURNS, role, 0, 0, -1);
}

int delegate_role_known(const char *project_root, const char *role)
{
   if (!role || !role[0])
      return 0;
   const char *canonical = delegate_role_canonicalize(role);
   if (!canonical || !canonical[0])
      return 0; /* the module could not name it, so neither can we */

   /* WHICH roles ship is the module's list, and that list is the permission
      table: a role with no entry there holds nothing. This used to be a copy of
      it living here, agreeing by nothing but coincidence. */
   if (role_policy_ask(DELEGATE_ROLE_OP_BUILTIN, canonical, 0, 0, 0))
      return 1;

   /* The other half is a filesystem lookup, which IS ours: a project- or
      user-level template file defines a custom role. Check the name as given and
      canonicalized, so an alias to a custom role still works. */
   char path[ROLE_TEMPLATE_PATH_MAX];
   return role_template_path(project_root, canonical, path, sizeof(path)) == 0 ||
          role_template_path(project_root, role, path, sizeof(path)) == 0;
}

const char *delegate_role_canonicalize(const char *role)
{
   if (!role || !role[0])
      return role;
   if (g_canonicalizer)
   {
      static _Thread_local char canonical[AIMEE_DELEGATES_ROLE_MAX + 1];
      if (g_canonicalizer(role, canonical, sizeof(canonical)) == 0)
         return canonical;
      return ""; /* required module failed: unknown role paths fail closed */
   }
   /* No provider, no answer. This used to fall back to a copy of the module's
      alias table, so the canonical spelling of a role depended on whether the
      module happened to be registered: one question, two answers. Failing closed
      makes an unregistered module a refusal rather than a quiet second opinion. */
   return "";
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
   /* The per-role turn cap is operator configuration: the `max_turns:` frontmatter
    * of the role template (edited under the Personas tab), defaulting to -1 =
    * INFINITE. A configured positive value flows through
    * delegate_apply_max_turns_policy as a cap; -1 leaves the role unbounded. No
    * hardcoded per-role caps. */
   if (!role || !role[0])
      return -1;
   return role_template_max_turns(delegate_role_canonicalize(role));
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

void delegate_apply_max_turns_cap(agent_config_t *cfg, const char *role, int cap)
{
   if (!cfg || !role || !role[0] || cap <= 0)
      return;

   const char *canonical_role = delegate_role_canonicalize(role);
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (!delegate_agent_supports_role(&cfg->agents[i], canonical_role))
         continue;
      if (cfg->agents[i].max_turns <= 0 || cfg->agents[i].max_turns > cap)
         cfg->agents[i].max_turns = cap;
   }
}
