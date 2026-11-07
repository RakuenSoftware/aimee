/* HTTP adapter fixtures expose no host catalog and install no module tools.
 * Link the real empty registry; any attempted install must fail this fixture
 * instead of silently substituting a fake buffering policy. */
#include <assert.h>
int agent_tools_builtin_name_reserved(const char *name)
{
   (void)name;
   assert(!"HTTP-only fixture unexpectedly attempted module tool installation");
   return 1;
}
int agent_tools_is_mcp_derived(const char *name)
{
   (void)name;
   assert(!"HTTP-only fixture unexpectedly attempted module tool installation");
   return 1;
}
