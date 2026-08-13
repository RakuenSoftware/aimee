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
   {"search", {"knowledge_write", NULL, NULL, NULL}},
   {"reason", {"knowledge_write", NULL, NULL, NULL}},
   {"plan", {"knowledge_write", NULL, NULL, NULL}},
   {"continuity", {"knowledge_write", NULL, NULL, NULL}},
   {"beat-check", {"knowledge_write", NULL, NULL, NULL}},
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

/* Definitions these tests hand over, and what the module answers for each.
 *
 * Recorded the same way as the role table and for the same reason: a C test
 * cannot call the module, and re-reading a permissions block here would be a
 * second parser to disagree with the real one. What is proved HERE is that the
 * definition reaches the module and its answer reaches every consumer; that the
 * text parses to these permissions is proved in
 * server-go/modules/delegates/roledefinition_test.go. */
typedef struct
{
   const char *definition;
   const char *permissions[4];
} stub_definition_t;

static const stub_definition_t k_definitions[] = {
    {"permissions:\n  - knowledge_write\n", {"knowledge_write", NULL, NULL, NULL}},
};

static int stub_answer(uint8_t *response, size_t response_cap, size_t *response_len,
                       const char *const *permissions)
{
   uint32_t count = 0;
   while (count < 4 && permissions[count])
      count++;

   aimee_delegates_wire_t w;
   aimee_delegates_wire_init(&w, response, response_cap);
   aimee_delegates_wire_u32(&w, AIMEE_DELEGATES_PERMS_RESPONSE_MAGIC);
   aimee_delegates_wire_u32(&w, count);
   for (uint32_t i = 0; i < count; i++)
   {
      aimee_delegates_wire_str(&w, permissions[i]);
      aimee_delegates_wire_str(&w, stub_enforced_at(permissions[i]));
      aimee_delegates_wire_u32(&w, 0u); /* nothing recorded here is scoped */
   }
   aimee_delegates_wire_u32(&w, 0u); /* nor unenforced */

   if (w.overflow)
      return -1;
   *response_len = w.len;
   return 0;
}

static int stub_permissions(const uint8_t *request, size_t request_len, uint8_t *response,
                            size_t response_cap, size_t *response_len)
{
   aimee_delegates_rd_t r = {request, request_len, 0, 0};
   aimee_delegates_rd_u32(&r); /* magic */
   aimee_delegates_rd_u32(&r); /* version */
   unsigned flags = aimee_delegates_rd_u32(&r);

   char role[128];
   aimee_delegates_rd_str(&r, role, sizeof(role));
   if (r.bad)
      return -1;

   /* A definition replaces the built-in, so what the ROLE ships with stops
      mattering. The recorded answer is keyed on the definition text. */
   if (flags & AIMEE_DELEGATES_PERMS_DEFINED)
   {
      char definition[2048];
      aimee_delegates_rd_str(&r, definition, sizeof(definition));
      if (r.bad)
         return -1;
      for (size_t i = 0; i < sizeof(k_definitions) / sizeof(k_definitions[0]); i++)
         if (strcmp(k_definitions[i].definition, definition) == 0)
            return stub_answer(response, response_cap, response_len,
                               k_definitions[i].permissions);
      fprintf(stderr,
              "delegate_permissions_stub: no recorded answer for the definition:\n%s"
              "Ask ResolveRolePermissions for it and add the row; do not guess.\n",
              definition);
      abort();
   }

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

   return stub_answer(response, response_cap, response_len, found->permissions);
}

void delegate_permissions_stub_install(void)
{
   delegate_register_permissions_provider(stub_permissions);
}
