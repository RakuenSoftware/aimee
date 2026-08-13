/* test_delegate_permissions.c: the C side of the resolved permission set.
 *
 * WHAT a role may do is the module's answer and is tested against the module.
 * What is tested here is the part C owns: reading that answer into a fixed-size
 * held set without ever quietly holding something different from what was sent.
 *
 * The wire reader truncates a string that does not fit and says nothing, and the
 * fill loops used to drop anything past the end of each array. For a list of
 * GRANTS that quietly removes powers; for the list of WITHHELD TOOLS it quietly
 * returns them. Both are now refusals.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/module_api.h>

/* What the next provider answers with. Set by each case. */
static int g_grant_count;
static const char *g_grant_name = "tools";
static int g_scope_count;
static int g_denied_count;
static const char *g_denied_name = "bash";

static int fake_permissions(const uint8_t *request, size_t request_len, uint8_t *response,
                            size_t response_cap, size_t *response_len)
{
   (void)request;
   (void)request_len;

   aimee_delegates_wire_t w;
   aimee_delegates_wire_init(&w, response, response_cap);
   aimee_delegates_wire_u32(&w, AIMEE_DELEGATES_PERMS_RESPONSE_MAGIC);
   aimee_delegates_wire_u32(&w, (uint32_t)g_grant_count);
   for (int i = 0; i < g_grant_count; i++)
   {
      aimee_delegates_wire_str(&w, g_grant_name);
      aimee_delegates_wire_str(&w, "tools");
      aimee_delegates_wire_u32(&w, (uint32_t)g_scope_count);
      for (int s = 0; s < g_scope_count; s++)
         aimee_delegates_wire_str(&w, "/srv/repo");
   }
   aimee_delegates_wire_u32(&w, 0u); /* unenforced */
   aimee_delegates_wire_u32(&w, (uint32_t)g_denied_count);
   for (int i = 0; i < g_denied_count; i++)
      aimee_delegates_wire_str(&w, g_denied_name);

   if (w.overflow)
      return -1;
   *response_len = w.len;
   return 0;
}

static void reset_answer(void)
{
   g_grant_count = 1;
   g_grant_name = "tools";
   g_scope_count = 0;
   g_denied_count = 1;
   g_denied_name = "bash";
}

static void test_an_answer_that_fits_is_held(void)
{
   reset_answer();
   delegate_permissions_t perms;
   assert(delegate_permissions_for_role("review", NULL, &perms) == 0);
   assert(delegate_permissions_has(&perms, "tools") == 1);
   assert(delegate_permissions_denies_tool(&perms, "bash") == 1);
   assert(delegate_permissions_denies_tool(&perms, "read_file") == 0);
   printf("  PASS: test_an_answer_that_fits_is_held\n");
}

/* More withheld tools than the held set can store used to drop the overflow,
 * which HANDS BACK the tools it could not remember. */
static void test_too_many_withheld_tools_is_a_refusal(void)
{
   reset_answer();
   g_denied_count = DELEGATE_PERM_TOOL_MAX + 1;

   delegate_permissions_t perms;
   assert(delegate_permissions_for_role("review", NULL, &perms) != 0);
   /* And it holds NOTHING rather than a shortened version of the answer. */
   assert(delegate_permissions_has(&perms, "tools") == 0);
   assert(perms.denied_tool_count == 0);
   printf("  PASS: test_too_many_withheld_tools_is_a_refusal\n");
}

static void test_too_many_permissions_is_a_refusal(void)
{
   reset_answer();
   g_grant_count = DELEGATE_PERM_MAX + 1;

   delegate_permissions_t perms;
   assert(delegate_permissions_for_role("review", NULL, &perms) != 0);
   assert(perms.count == 0);
   printf("  PASS: test_too_many_permissions_is_a_refusal\n");
}

static void test_too_many_scopes_is_a_refusal(void)
{
   reset_answer();
   g_grant_name = "repo_write";
   g_scope_count = DELEGATE_PERM_SCOPE_MAX + 1;

   delegate_permissions_t perms;
   assert(delegate_permissions_for_role("code", NULL, &perms) != 0);
   /* Dropping scopes narrows a grant, which is the safe direction and still the
      wrong answer: the operator listed repositories this build cannot hold. */
   assert(perms.count == 0);
   printf("  PASS: test_too_many_scopes_is_a_refusal\n");
}

/* A name too long for the held set is a DIFFERENT name once truncated, so the
 * permission it describes is silently not the one that was granted. */
static void test_a_name_that_does_not_fit_is_a_refusal(void)
{
   reset_answer();
   static char huge[DELEGATE_PERM_NAME_MAX + 8];
   memset(huge, 'x', sizeof(huge) - 1);
   huge[sizeof(huge) - 1] = '\0';
   g_grant_name = huge;

   delegate_permissions_t perms;
   assert(delegate_permissions_for_role("custom", NULL, &perms) != 0);
   assert(perms.count == 0);
   printf("  PASS: test_a_name_that_does_not_fit_is_a_refusal\n");
}

/* No provider is the same refusal: a delegate whose powers cannot be
 * established holds none of them. */
static void test_no_provider_holds_nothing(void)
{
   delegate_register_permissions_provider(NULL);
   delegate_permissions_t perms;
   assert(delegate_permissions_for_role("code", NULL, &perms) != 0);
   assert(delegate_permissions_has(&perms, "repo_write") == 0);
   delegate_register_permissions_provider(fake_permissions);
   printf("  PASS: test_no_provider_holds_nothing\n");
}

int main(void)
{
   delegate_register_permissions_provider(fake_permissions);
   printf("test_delegate_permissions\n");
   test_an_answer_that_fits_is_held();
   test_too_many_withheld_tools_is_a_refusal();
   test_too_many_permissions_is_a_refusal();
   test_too_many_scopes_is_a_refusal();
   test_a_name_that_does_not_fit_is_a_refusal();
   test_no_provider_holds_nothing();
   printf("All tests passed.\n");
   return 0;
}
