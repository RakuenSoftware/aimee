/* test_command_registry.c: the invariant is that registration is the ONLY way in.
 *
 * These tests are about routing identity, not plumbing: a name resolves one way or
 * it does not resolve. The bugs they exist to prevent were all found on
 * 2026-08-11, when capability surface was declared four separate times by hand --
 * a command reachable from the CLI but absent from the surface an agent is shown,
 * two different lists both called "core", and standing guidance naming tools no
 * client could call. */
#include "command_registry.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct cJSON *stub_fn(const struct cJSON *args, void *ud)
{
   (void)args;
   (void)ud;
   return cJSON_CreateObject();
}

static aimee_command_t mk(const char *group, const char *verb, unsigned surfaces)
{
   aimee_command_t c;
   memset(&c, 0, sizeof c);
   c.group = group;
   c.verb = verb;
   c.summary = "test";
   c.surfaces = surfaces;
   c.mcp_visibility = AIMEE_MCP_PROMINENT;
   c.fn = stub_fn;
   c.module = "test";
   return c;
}

static void test_register_and_find(void)
{
   aimee_command_registry_reset();
   aimee_command_t c = mk("memory", "get", AIMEE_SURFACE_ALL);
   assert(aimee_command_register(&c) == 0);
   assert(aimee_command_count() == 1);

   /* Both spellings resolve to the SAME entry: `aimee memory get` and the RPC
    * "memory.get" are one command, which is the property that stops a surface
    * from having its own idea of what exists. */
   const aimee_command_t *by_pair = aimee_command_find("memory", "get");
   const aimee_command_t *by_method = aimee_command_find_method("memory.get");
   assert(by_pair && by_method && by_pair == by_method);
   printf("register_and_find OK\n");
}

/* Two modules claiming one name is a build-order-dependent bug. Refuse it rather
 * than let the last registrant silently win. */
static void test_duplicate_refused(void)
{
   aimee_command_registry_reset();
   aimee_command_t a = mk("memory", "get", AIMEE_SURFACE_ALL);
   aimee_command_t b = mk("memory", "get", AIMEE_SURFACE_ALL);
   assert(aimee_command_register(&a) == 0);
   assert(aimee_command_register(&b) == -1);
   assert(aimee_command_count() == 1);
   printf("duplicate_refused OK\n");
}

/* A command on no surface answers nothing; keeping it would put an entry in the
 * table that cannot be reached, which is exactly the state this replaces. */
static void test_no_surface_refused(void)
{
   aimee_command_registry_reset();
   aimee_command_t c = mk("memory", "get", 0);
   assert(aimee_command_register(&c) == -1);
   assert(aimee_command_count() == 0);
   printf("no_surface_refused OK\n");
}

/* The dotted form must parse one way, so a component carrying a dot or a space is
 * refused at registration rather than resolving ambiguously later. */
static void test_malformed_names_refused(void)
{
   aimee_command_registry_reset();
   aimee_command_t dotted = mk("mem.ory", "get", AIMEE_SURFACE_ALL);
   aimee_command_t spaced = mk("memory", "get all", AIMEE_SURFACE_ALL);
   aimee_command_t upper = mk("Memory", "get", AIMEE_SURFACE_ALL);
   aimee_command_t empty = mk("memory", "", AIMEE_SURFACE_ALL);
   assert(aimee_command_register(&dotted) == -1);
   assert(aimee_command_register(&spaced) == -1);
   assert(aimee_command_register(&upper) == -1);
   assert(aimee_command_register(&empty) == -1);
   assert(aimee_command_count() == 0);
   printf("malformed_names_refused OK\n");
}

/* THE INVARIANT. An unregistered command is unroutable from EVERY surface, not
 * just the one that forgot it. Before the registry, `memory get` existed on the
 * CLI while the agent's tool surface had no idea it did. */
static void test_unregistered_is_unroutable_everywhere(void)
{
   aimee_command_registry_reset();
   assert(aimee_command_find("memory", "get") == NULL);
   assert(aimee_command_find_method("memory.get") == NULL);
   printf("unregistered_is_unroutable_everywhere OK\n");
}

/* A surface builds its view by ENUMERATING the registry, never by keeping a list.
 * This is what a tools/list or a CLI help builder does. */
static void test_surface_view_is_derived(void)
{
   aimee_command_registry_reset();
   aimee_command_t a = mk("memory", "get", AIMEE_SURFACE_ALL);
   aimee_command_t b = mk("memory", "search", AIMEE_SURFACE_CLI | AIMEE_SURFACE_RPC);
   aimee_command_t c = mk("session", "search", AIMEE_SURFACE_MCP);
   assert(aimee_command_register(&a) == 0);
   assert(aimee_command_register(&b) == 0);
   assert(aimee_command_register(&c) == 0);

   int mcp = 0, cli = 0;
   for (size_t i = 0; i < aimee_command_count(); i++)
   {
      const aimee_command_t *cmd = aimee_command_at(i);
      if (cmd->surfaces & AIMEE_SURFACE_MCP)
         mcp++;
      if (cmd->surfaces & AIMEE_SURFACE_CLI)
         cli++;
   }
   assert(mcp == 2); /* memory.get + session.search */
   assert(cli == 2); /* memory.get + memory.search */
   assert(aimee_command_at(aimee_command_count()) == NULL);
   printf("surface_view_is_derived OK\n");
}

int main(void)
{
   printf("test_command_registry:\n");
   test_register_and_find();
   test_duplicate_refused();
   test_no_surface_refused();
   test_malformed_names_refused();
   test_unregistered_is_unroutable_everywhere();
   test_surface_view_is_derived();
   aimee_command_registry_reset();
   printf("all command_registry tests passed\n");
   return 0;
}
