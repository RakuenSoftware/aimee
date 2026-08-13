/* Registers the delegates module's answers into the REAL delegate_role.c seam.
 *
 * For suites that link modules/delegates/delegate_role.o and exercise code which
 * canonicalizes a role or asks whether one ships. Those functions have no
 * built-in answer: with no provider they fail closed, because the fallback that
 * used to sit there was a copy of the module's tables that agreed with them by
 * coincidence. A test that needs the answers registers them, from here.
 *
 * These are RECORDINGS, pinned against the module in
 * server-go/modules/delegates/rolepolicy_test.go and permissions_test.go. An
 * unlisted alias resolves to itself, which is what the module does; an unlisted
 * role is not built in, which is also what the module does. Nothing here decides
 * either.
 *
 * Do not link this alongside delegate_role_policy_stub.c: that one REPLACES the
 * seam rather than filling it, and the two would collide.
 */

#include "delegate_role_seam_stub.h"

#include <aimee/delegates/delegate_role.h>

#include <stdio.h>
#include <string.h>

static const struct
{
   const char *alias;
   const char *canonical;
} k_aliases[] = {
    {"implement", "code"},          {"build", "code"},
    {"reviewer", "review"},         {"verifier", "validate"},
    {"test", "validate"},           {"check", "validate"},
    {"evaluate", "validate"},       {"evaluate-optimize", "validate"},
    {"inspect", "diagnose"},        {"research", "execute"},
    {"enforce", "execute"},         {"recall", "search"},
    {"synthesize", "summarize"},    {"rank-fuse", "reason"},
    {"classify-score", "reason"},   {"planner", "plan"},
    {"planning", "plan"},
};

static const char *const k_builtin_roles[] = {
    "review",  "validate", "diagnose",   "code",       "refactor", "explain", "draft",  "execute",
    "summarize", "format", "search",     "reason",     "plan",     "continuity", "beat-check"};

static const char *canonical_of(const char *role)
{
   for (size_t i = 0; i < sizeof(k_aliases) / sizeof(k_aliases[0]); i++)
      if (strcmp(role, k_aliases[i].alias) == 0)
         return k_aliases[i].canonical;
   return role;
}

static int seam_canonicalize(const char *role, char *out, size_t cap)
{
   if (!role || !role[0] || !out)
      return -1;
   snprintf(out, cap, "%s", canonical_of(role));
   return 0;
}

static int seam_policy(int op, const char *role, int a, int b, int c, int *out)
{
   (void)a;
   (void)b;
   (void)c;
   if (op != DELEGATE_ROLE_OP_BUILTIN || !out || !role)
      return -1;
   const char *canonical = canonical_of(role);
   *out = 0;
   for (size_t i = 0; i < sizeof(k_builtin_roles) / sizeof(k_builtin_roles[0]); i++)
      if (strcmp(canonical, k_builtin_roles[i]) == 0)
      {
         *out = 1;
         break;
      }
   return 0;
}

void delegate_role_seam_install(void)
{
   delegate_role_register_canonicalizer(seam_canonicalize);
   delegate_register_role_policy_provider(seam_policy);
}
