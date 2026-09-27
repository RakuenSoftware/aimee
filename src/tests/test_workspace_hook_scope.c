#include "workspace_hook_scope.h"
#include <assert.h>
#include <stdio.h>
static const char *roots[] = {"/registered/repo", "/other/"};
static int count;
int config_workspace_count(void)
{
   return count;
}
const char *config_workspaces(int i)
{
   return roots[i];
}
int main(void)
{
   assert(!workspace_hook_registered("/registered/repo"));
   count = 2;
   assert(workspace_hook_registered("/registered/repo"));
   assert(workspace_hook_registered("/registered/repo/src"));
   assert(workspace_hook_registered("/other/src"));
   assert(!workspace_hook_registered("/registered/repo-other"));
   assert(!workspace_hook_registered("/unregistered"));
   assert(!workspace_hook_registered(NULL));
   puts("workspace hook scope: ok");
   return 0;
}
