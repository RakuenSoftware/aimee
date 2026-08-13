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

/* Which permission each tool needs, as the module answers it.
 *
 * Recorded for the same reason as stub_enforced_at above, and pinned against the
 * module in server-go/modules/delegates/toolpermissions_test.go. A tool absent
 * from this table needs nothing beyond `tools`, which is the module's rule too:
 * reading is implicit. */
static const struct
{
   const char *tool;
   const char *permission;
} k_tool_permissions[] = {
    {"bash", "shell"},
    {"execute_script", "shell"},
    {"run_background_process", "shell"},
    {"get_background_output", "shell"},
    {"kill_background_process", "shell"},
    {"list_background_processes", "shell"},
    {"write_file", "repo_write"},
    {"edit_file", "repo_write"},
    {"git_commit", "repo_write"},
    {"git_push", "repo_write"},
    {"git_branch", "repo_write"},
    {"git_pr", "repo_write"},
    {"create_note", "knowledge_write"},
    {"record_attempt", "knowledge_write"},
};

/* The tools a held set withholds. */
static void stub_write_denied(aimee_delegates_wire_t *w, const char *const *permissions)
{
   const char *denied[sizeof(k_tool_permissions) / sizeof(k_tool_permissions[0])];
   uint32_t count = 0;
   for (size_t i = 0; i < sizeof(k_tool_permissions) / sizeof(k_tool_permissions[0]); i++)
   {
      int held = 0;
      for (int j = 0; !held && j < 4 && permissions[j]; j++)
         held = strcmp(permissions[j], k_tool_permissions[i].permission) == 0;
      if (!held)
         denied[count++] = k_tool_permissions[i].tool;
   }
   aimee_delegates_wire_u32(w, count);
   for (uint32_t i = 0; i < count; i++)
      aimee_delegates_wire_str(w, denied[i]);
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
   /* Sorted, as the module returns them. */
   const char *permissions[4];
   /* Scopes for permissions[0], when the recorded answer has any. One is enough
      for what these fixtures ask; a second would want its own column. */
   const char *scopes[2];
   /* Where each permission is enforced, when the answer is not the built-in
      default -- an operator's own point, which is the case worth recording. NULL
      falls back to stub_enforced_at. */
   const char *points[4];
} stub_definition_t;

static const stub_definition_t k_definitions[] = {
    {"permissions:\n  - knowledge_write\n",
     {"knowledge_write", NULL, NULL, NULL},
     {NULL, NULL},
     {NULL, NULL, NULL, NULL}},
    {"permissions:\n  - name: repo_write\n    scopes: [/srv/repo-a]\n",
     {"repo_write", NULL, NULL, NULL},
     {"/srv/repo-a", NULL},
     {NULL, NULL, NULL, NULL}},
    {"permissions:\n  - tools\n  - name: deploy\n    enforced_at: deploy-gate\n",
     {"deploy", "tools", NULL, NULL},
     {NULL, NULL},
     {"deploy-gate", NULL, NULL, NULL}},
};

static int stub_answer(uint8_t *response, size_t response_cap, size_t *response_len,
                       const char *const *permissions, const char *const *scopes,
                       const char *const *points)
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
      aimee_delegates_wire_str(&w, (points && points[i]) ? points[i]
                                                         : stub_enforced_at(permissions[i]));

      uint32_t scope_count = 0;
      if (i == 0 && scopes)
         while (scope_count < 2 && scopes[scope_count])
            scope_count++;
      aimee_delegates_wire_u32(&w, scope_count);
      for (uint32_t j = 0; j < scope_count; j++)
         aimee_delegates_wire_str(&w, scopes[j]);
   }
   aimee_delegates_wire_u32(&w, 0u); /* nothing recorded here is unenforced */
   stub_write_denied(&w, permissions);

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
                               k_definitions[i].permissions, k_definitions[i].scopes,
                               k_definitions[i].points);
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

   /* The built-ins ship unscoped. */
   return stub_answer(response, response_cap, response_len, found->permissions, NULL, NULL);
}

void delegate_permissions_stub_install(void)
{
   delegate_register_permissions_provider(stub_permissions);
}
