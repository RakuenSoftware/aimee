/* A permissions provider for tests that do not run the module.
 *
 * The table below is the module's OWN answer for each built-in role, generated
 * from ResolveRolePermissions rather than transcribed by hand. It is a recording,
 * not a second copy of the rule: nothing here decides what a role may do, and a
 * role the module changes will disagree with this file loudly rather than
 * quietly, because an unlisted role aborts.
 *
 * Suites that exercise the real module must not link this. It exists so a test
 * about worktree layout or container specs does not have to stand up a module
 * process to find out that `code` may write.
 */

#include "delegate_permissions_stub.h"

#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/module_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   const char *role;
   const char *permissions[4];
} stub_role_t;

/* Recorded from the delegates module. To refresh a row, ask the module rather
 * than reasoning it out here: ResolveRolePermissions(role, nil).Names() in
 * server-go/modules/delegates returns exactly this list, sorted. */
static const stub_role_t k_roles[] = {
   {"review", {"tools", NULL, NULL, NULL}},
   {"validate", {"knowledge_write", "shell", "tools", NULL}},
   {"diagnose", {"shell", "tools", NULL, NULL}},
   {"code", {"knowledge_write", "repo_write", "shell", "tools"}},
   {"refactor", {"knowledge_write", "repo_write", "shell", "tools"}},
   {"explain", {"knowledge_write", NULL, NULL, NULL}},
   {"draft", {"knowledge_write", NULL, NULL, NULL}},
   {"execute", {"knowledge_write", "shell", "tools", NULL}},
   {"summarize", {"knowledge_write", NULL, NULL, NULL}},
   {"format", {"knowledge_write", NULL, NULL, NULL}},
   {"search", {"knowledge_write", "tools", NULL, NULL}},
   {"reason", {"knowledge_write", NULL, NULL, NULL}},
   {"plan", {"knowledge_write", NULL, NULL, NULL}},
   {"continuity", {"knowledge_write", "tools", NULL, NULL}},
   {"beat-check", {"knowledge_write", "tools", NULL, NULL}},
};

/* Where each shipped permission is evaluated. Recorded for the same reason as
 * the table above: no test should be the place this is decided. */
static const char *stub_enforced_at(const char *permission)
{
   if (strcmp(permission, "repo_write") == 0)
      return "mount";
   if (strcmp(permission, "knowledge_write") == 0)
      return "api";
   return "tools";
}

static int stub_permissions(const uint8_t *request, size_t request_len, uint8_t *response,
                            size_t response_cap, size_t *response_len)
{
   aimee_delegates_rd_t r = {request, request_len, 0, 0};
   aimee_delegates_rd_u32(&r); /* magic */
   aimee_delegates_rd_u32(&r); /* version */
   aimee_delegates_rd_u32(&r); /* flags */

   char role[128];
   aimee_delegates_rd_str(&r, role, sizeof(role));
   if (r.bad)
      return -1;

   const stub_role_t *found = NULL;
   for (size_t i = 0; i < sizeof(k_roles) / sizeof(k_roles[0]); i++)
      if (strcmp(k_roles[i].role, role) == 0)
         found = &k_roles[i];

   /* An unknown role means the module answers something this file has never
    * seen. Guessing would let a real behaviour change pass as a green test. */
   if (!found)
   {
      fprintf(stderr,
              "delegate_permissions_stub: no recorded answer for role '%s'. Regenerate this "
              "table from the delegates module.\n",
              role);
      abort();
   }

   uint32_t count = 0;
   while (count < 4 && found->permissions[count])
      count++;

   aimee_delegates_wire_t w;
   aimee_delegates_wire_init(&w, response, response_cap);
   aimee_delegates_wire_u32(&w, AIMEE_DELEGATES_PERMS_RESPONSE_MAGIC);
   aimee_delegates_wire_u32(&w, count);
   for (uint32_t i = 0; i < count; i++)
   {
      aimee_delegates_wire_str(&w, found->permissions[i]);
      aimee_delegates_wire_str(&w, stub_enforced_at(found->permissions[i]));
      aimee_delegates_wire_u32(&w, 0u); /* the built-ins ship unscoped */
   }
   aimee_delegates_wire_u32(&w, 0u); /* nothing shipped is unenforced */

   if (w.overflow)
      return -1;
   *response_len = w.len;
   return 0;
}

void delegate_permissions_stub_install(void)
{
   delegate_register_permissions_provider(stub_permissions);
}
