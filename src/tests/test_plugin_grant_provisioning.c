/* test_plugin_grant_provisioning.c: what the provisioner writes, the daemon reads.
 *
 * scripts/provision-plugin-module.py emits a .grant file, and bus_runtime.c
 * parses it. Nothing checked that those two agree. A format the daemon silently
 * rejects is the worst shape this can fail in: the policy loads as "empty", the
 * listener runs and denies everything, and the module's own logs say only
 * "attach denied" -- with no indication that its grant was never understood.
 *
 * So this runs the real provisioner and hands its output to the real parser.
 *
 * Usage: unit-test-plugin-grant-provisioning /abs/path/to/provision-plugin-module.py
 */
#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_runtime.h>

#include "platform_test_util.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* MUST match the provisioner and server-go/modules/mcp/mcp.go. */
/* Kinds are DERIVED from the principal ref by the canonical module rule, so the
 * grant the provisioner writes must agree with that rule exactly -- this is the
 * check that the two allocation authorities really did become one. */
#define PLUGIN_KIND(ref, stage) (4096u + (ref) * 256u + (stage))
#define PLUGIN_REF_FIRST        200u
#define PLUGIN_REF_LIMIT        456u
#define STAGE_INVOKE            1u
#define STAGE_DECLARE           2u
#define EGRESS_EVENT_FIRST      12291u
#define EGRESS_EVENT_LAST       12294u
#define EGRESS_CLIENT_OFFSET    512u

static void run_provisioner(const char *tool, const char *cfg, const char *instance,
                            const char *module_bin)
{
   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "python3 '%s' --instance %s --argv '[\"python3\",\"/opt/p.py\"]' "
            "--module-bin '%s' --config-dir '%s' >/dev/null 2>&1",
            tool, instance, module_bin, cfg);
   int rc = system(cmd);
   assert(rc == 0 && "the provisioner must succeed");
}

int main(int argc, char **argv)
{
   /* Default to the in-tree provisioner so this runs in the ordinary suite with
    * no arguments and no infrastructure beyond python3; a target that only runs
    * behind a special invocation is one that quietly stops running. */
   const char *script = argc == 2 ? argv[1] : "../scripts/provision-plugin-module.py";
   char tool[PATH_MAX];
   if (!realpath(script, tool))
   {
      fprintf(stderr, "cannot resolve %s (run from src/, or pass the path)\n", script);
      return 1;
   }

   char cfg[256];
   snprintf(cfg, sizeof(cfg), "%s/aimee-grant-prov-XXXXXX", platform_tmpdir());
   assert(mkdtemp(cfg) != NULL);

   /* The grant records an executable path and the loader canonicalises it, so it
    * has to exist. */
   char module_bin[PATH_MAX];
   snprintf(module_bin, sizeof(module_bin), "%s/aimee-module", cfg);
   FILE *f = fopen(module_bin, "w");
   assert(f && fputs("#!/bin/sh\n", f) >= 0 && fclose(f) == 0);
   assert(chmod(module_bin, 0755) == 0);

   run_provisioner(tool, cfg, "github", module_bin);
   run_provisioner(tool, cfg, "jira", module_bin);

   char policy_dir[512];
   snprintf(policy_dir, sizeof(policy_dir), "%s/modules.d/server", cfg);

   bus_runtime_policy_t *policy = NULL;
   assert(bus_runtime_policy_load_dir(policy_dir, &policy) == 0 &&
          "the daemon's parser must accept what the provisioner wrote");
   assert(policy != NULL);

   size_t count = 0;
   const bus_runtime_grant_t *grants = bus_runtime_policy_grants(policy, &count);
   assert(grants != NULL);
   assert(count == 4 && "each provisioned instance must load serving and egress grants");

   /* Every instance must be a DISTINCT identity: a shared principal_ref means a
    * shared grant, and a shared event kind means the second module is denied at
    * attach by bus_host_serve_kind(). */
   for (size_t i = 0; i < count; i++)
   {
      assert(grants[i].principal_class == 1);
      assert(grants[i].principal_ref != 0);
      int serving = grants[i].serve_count == 2;
      if (serving)
      {
         assert(grants[i].serve[1] == grants[i].serve[0] + 1 && "the pair must be adjacent");
         assert(grants[i].principal_ref >= PLUGIN_REF_FIRST &&
                grants[i].principal_ref < PLUGIN_REF_LIMIT &&
                "the ref must come from the reserved plugin band");
         assert(grants[i].serve[0] == PLUGIN_KIND(grants[i].principal_ref, STAGE_INVOKE) &&
                "the invoke kind must be derived from the principal ref");
         assert(grants[i].serve[1] == PLUGIN_KIND(grants[i].principal_ref, STAGE_DECLARE) &&
                "the declare kind must be derived from the principal ref");
      }
      else
      {
         assert(grants[i].serve_count == 0);
         assert(grants[i].principal_ref >= PLUGIN_REF_FIRST + EGRESS_CLIENT_OFFSET &&
                grants[i].principal_ref < PLUGIN_REF_LIMIT + EGRESS_CLIENT_OFFSET);
         assert(grants[i].request_count == EGRESS_EVENT_LAST - EGRESS_EVENT_FIRST + 1);
         for (size_t event = 0; event < grants[i].request_count; event++)
            assert(grants[i].request[event] == EGRESS_EVENT_FIRST + event);
      }
      /* db1 holds the highest canonical ref (30), so its block ends at
       * 4096 + 30*256 + 255. A plugin kind at or below that would sit inside a
       * canonical module's block -- the regression this derivation removes. */
      if (serving)
         assert(grants[i].serve[0] > 4096u + 32u * 256u + 255u &&
                "a plugin kind must not land in a canonical module's block");
      /* A plugin module SERVES; it is granted no publish/subscribe/request. A
       * serving grant that also requested things would be a wider blast radius
       * than the design claims. */
      assert(grants[i].publish_count == 0);
      assert(grants[i].subscribe_count == 0);
      if (serving)
         assert(grants[i].request_count == 0);
      assert(grants[i].executable && grants[i].executable[0] == '/');

      for (size_t j = i + 1; j < count; j++)
      {
         assert(grants[i].principal_ref != grants[j].principal_ref);
         for (size_t a = 0; a < grants[i].serve_count; a++)
            for (size_t b = 0; b < grants[j].serve_count; b++)
               assert(grants[i].serve[a] != grants[j].serve[b]);
      }
   }

   bus_runtime_policy_free(&policy);
   assert(policy == NULL);

   char rm[512];
   snprintf(rm, sizeof(rm), "rm -rf '%s'", cfg);
   (void)system(rm);

   printf("provisioned grants load in the daemon's own parser, with distinct identities\n");
   printf("all plugin grant provisioning tests passed\n");
   return 0;
}
