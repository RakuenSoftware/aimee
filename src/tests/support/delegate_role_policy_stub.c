#include <string.h>

static const char *test_delegate_policy_role(const char *role)
{
   if (!role || !role[0])
      return NULL;
   if (strcmp(role, "reviewer") == 0)
      return "review";
   if (strcmp(role, "verifier") == 0)
      return "validate";
   if (strcmp(role, "test") == 0 || strcmp(role, "check") == 0)
      return "validate";
   if (strcmp(role, "inspect") == 0)
      return "diagnose";
   return role;
}

/* Which roles want the parent worktree's diff. Like the rest of this harness
 * this MIRRORS the module's list rather than asking it, because these tests link
 * no bus; the list itself is pinned against the module in
 * server-go/modules/delegates/rolepolicy_test.go. A test that cares about the
 * distinction registers its own provider instead. */
int delegate_role_needs_parent_diff(const char *role)
{
   role = test_delegate_policy_role(role);
   return role && (strcmp(role, "review") == 0 || strcmp(role, "validate") == 0 ||
                   strcmp(role, "diagnose") == 0);
}

/* The shape of work a role does, as task_type_t. Mirrors the module's map for
 * the same reason as the rest of this file; the map itself is pinned against
 * the module in server-go/modules/delegates/rolepolicy_test.go. */
int delegate_role_task_shape(const char *role)
{
   role = test_delegate_policy_role(role);
   if (!role)
      return 0; /* TASK_TYPE_GENERAL */
   if (strcmp(role, "review") == 0)
      return 4; /* TASK_TYPE_REVIEW */
   if (strcmp(role, "diagnose") == 0)
      return 1; /* TASK_TYPE_BUG_FIX */
   if (strcmp(role, "refactor") == 0)
      return 2; /* TASK_TYPE_REFACTOR */
   if (strcmp(role, "code") == 0)
      return 3; /* TASK_TYPE_FEATURE */
   if (strcmp(role, "validate") == 0 || strcmp(role, "test") == 0)
      return 5; /* TASK_TYPE_TEST */
   return 0;
}

int delegate_auto_tools_for_invocation(int holds_tools, int max_turns, int explicit_tools)
{
   if (explicit_tools)
      return 1;
   if (max_turns == 1)
      return 0;
   return holds_tools;
}

const char *delegate_role_canonicalize(const char *role)
{
   const char *canonical = test_delegate_policy_role(role);
   return canonical ? canonical : role;
}

int delegate_default_max_turns_for_role(const char *role)
{
   role = test_delegate_policy_role(role);
   if (!role)
      return -1;
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
   role = test_delegate_policy_role(role);
   if (!role)
      return -1;
   if (strcmp(role, "validate") == 0)
      return 8;
   if (strcmp(role, "search") == 0)
      return 10;
   return -1;
}

int delegate_role_is_write(const char *role)
{
   return role && (strcmp(role, "code") == 0 || strcmp(role, "refactor") == 0);
}

/* Dispatch now refuses an unknown role and an unknown persona. The worker under
 * test only needs the accept/reject decision, so keep the stub's knowledge to the
 * roles and personas its fixtures actually use, plus explicit rejects for the
 * culled names. */
const char *delegate_role_removed_reason(const char *role)
{
   if (!role || !role[0])
      return NULL;
   if (strcmp(role, "prose") == 0 || strcmp(role, "line-edit") == 0 ||
       strcmp(role, "lyric") == 0 || strcmp(role, "hook") == 0 ||
       strcmp(role, "prosody") == 0 || strcmp(role, "songform") == 0)
      return "removed: use a persona with a general role such as draft, review or validate.";
   return NULL;
}

int delegate_role_known(const char *project_root, const char *role)
{
   (void)project_root;
   if (!role || !role[0])
      return 0;
   if (delegate_role_removed_reason(role))
      return 0;
   role = test_delegate_policy_role(role);
   static const char *const known[] = {"review",  "validate", "diagnose",   "code",
                                       "refactor", "explain",  "draft",     "execute",
                                       "summarize", "format",  "search",    "reason",
                                       "plan",     "continuity", "beat-check", NULL};
   for (int i = 0; known[i]; i++)
      if (strcmp(role, known[i]) == 0)
         return 1;
   return 0;
}

int persona_exists(const char *project_root, const char *name)
{
   (void)project_root;
   return name && name[0] && strcmp(name, "nosuchpersona") != 0;
}
