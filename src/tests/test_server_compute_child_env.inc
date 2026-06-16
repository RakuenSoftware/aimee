/* test_server_compute_child_env.inc: delegate_child_export_context_env coverage.
 * Split out of test_server_compute.c to keep it under the 2000-line hard limit.
 * Included into test_server_compute.c so it shares the server_compute.c
 * translation unit (reaches tl_delegation_depth / tl_parent_delegation_id). */

/* delegate_child_export_context_env mirrors the forking thread's TLS depth/parent
 * into the env for a post-fork child. It must export the THREAD's values (not a
 * concurrently-clobbered global) when inside a delegate, and leave the env alone
 * for a primary agent (depth 0, no parent). */
static void test_child_export_context_env(void)
{
   /* Pre-seed the env with a stale value as if a concurrent delegate had written
    * it; inside a delegate the export must override with this thread's TLS. */
   platform_setenv("AIMEE_DELEGATE_DEPTH", "9");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "stale-parent");

   tl_delegation_depth = 3;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "deleg-self");
   delegate_child_export_context_env();
   assert(strcmp(getenv("AIMEE_DELEGATE_DEPTH"), "3") == 0);
   assert(strcmp(getenv("AIMEE_PARENT_DELEGATION_ID"), "deleg-self") == 0);

   /* Primary agent: depth 0, no parent -> no override (inherited env preserved). */
   platform_setenv("AIMEE_DELEGATE_DEPTH", "7");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "inherited");
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';
   delegate_child_export_context_env();
   assert(strcmp(getenv("AIMEE_DELEGATE_DEPTH"), "7") == 0);
   assert(strcmp(getenv("AIMEE_PARENT_DELEGATION_ID"), "inherited") == 0);

   platform_setenv("AIMEE_DELEGATE_DEPTH", "");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "");
}
