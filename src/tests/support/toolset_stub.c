#include "toolset.h"
#include <stdio.h>
#include <string.h>

void toolset_registry_init(toolset_registry_t *registry)
{
   if (!registry)
      return;
   memset(registry, 0, sizeof(*registry));
   snprintf(registry->script_allowed_tools, sizeof(registry->script_allowed_tools), "script_rpc");
}

int toolset_registry_load_effective(toolset_registry_t *registry, char *err, size_t err_len)
{
   (void)err;
   (void)err_len;
   toolset_registry_init(registry);
   return 0;
}

const toolset_def_t *toolset_registry_find(const toolset_registry_t *registry, const char *name)
{
   (void)name;
   if (!registry)
      return NULL;
   return NULL;
}

int toolset_registry_validate(const toolset_registry_t *registry, char *err, size_t err_len)
{
   (void)registry;
   (void)err;
   (void)err_len;
   return 0;
}

int toolset_resolve(const toolset_registry_t *registry, const char *name,
                    char out[][TOOLSET_TOOL_MAX], int max_tools, char *err, size_t err_len)
{
   (void)registry;
   (void)name;
   (void)out;
   (void)max_tools;
   if (err_len > 0)
      snprintf(err, err_len, "toolset stub");
   return -1;
}

int toolset_resolve_effective(const char *name, char out[][TOOLSET_TOOL_MAX], int max_tools,
                              char *err, size_t err_len)
{
   toolset_registry_t registry;
   toolset_registry_init(&registry);
   return toolset_resolve(&registry, name, out, max_tools, err, err_len);
}

int toolset_tool_known(const char *name)
{
   return name && name[0];
}

const char *toolset_for_delegate_role(const char *role)
{
   (void)role;
   return NULL;
}
