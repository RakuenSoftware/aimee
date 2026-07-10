/* test_wfe_custom.c -- config-extensible blocks: registry load/validate, custom
 * block type-checking, and exec_custom (command opt-in) through the engine. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_blocks.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

static char g_home[64];

static void write_file(const char *path, const char *body, mode_t mode)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(body, f);
   fclose(f);
   chmod(path, mode);
}

/* block-style YAML (aimee's yaml.c does not support flow [] sequences). */
static const char *GOOD_BLOCKS = "allow_command: true\n"
                                 "blocks:\n"
                                 "  - name: lint\n"
                                 "    consumes: branch\n"
                                 "    produces: branch\n"
                                 "    executor: command\n"
                                 "    command:\n"
                                 "      - /bin/true\n"
                                 "  - name: notify\n"
                                 "    consumes: none\n"
                                 "    produces: none\n"
                                 "    executor: command\n"
                                 "    command:\n"
                                 "      - /bin/true\n";

static const char *blocks_path(void)
{
   static char p[256];
   snprintf(p, sizeof p, "%s/workflows/blocks.yaml", g_home);
   return p;
}

int main(void)
{
   printf("wfe-custom: ");
   strcpy(g_home, "/tmp/wfe_cust_XXXXXX");
   assert(wfe_test_mkdtemp(g_home));
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", g_home);
   mkdir(wf, 0755);
   setenv("AIMEE_HOME", g_home, 1);

   char err[256];

   /* --- valid registry loads; lookup + node-aware typing --- */
   write_file(blocks_path(), GOOD_BLOCKS, 0600);
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) == 0);
   assert(wfe_custom_count() == 2);
   assert(wfe_custom_commands_allowed() == 1);
   const wfe_custom_block_t *lint = wfe_custom_lookup("lint");
   assert(lint && lint->consumes == WFE_ART_BRANCH && lint->produces == WFE_ART_BRANCH);
   assert(wfe_block_from_name("lint") == WFE_BLK_CUSTOM);
   assert(wfe_block_from_name("notify") == WFE_BLK_CUSTOM);

   /* --- reject paths (each fails the load) --- */
   wfe_custom_registry_reset();
   write_file(blocks_path(),
              "blocks:\n  - name: merge\n    consumes: pr\n    produces: none\n"
              "    executor: command\n    command:\n      - /bin/true\n",
              0600); /* shadows built-in */
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) != 0);

   write_file(blocks_path(),
              "blocks:\n  - name: x\n    consumes: branch\n    produces: verdict\n"
              "    executor: delegate\n    persona: security\n    prompt: hi\n",
              0600); /* trust-bearing produces forbidden */
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) != 0);

   write_file(blocks_path(),
              "blocks:\n  - name: x\n    consumes: branch\n    produces: branch\n"
              "    executor: nonsense\n",
              0600); /* bad executor */
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) != 0);

   /* world/group-writable registry is refused */
   write_file(blocks_path(), GOOD_BLOCKS, 0666);
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) != 0);

   /* duplicate custom block name is rejected */
   write_file(blocks_path(),
              "blocks:\n  - name: dup\n    consumes: none\n    produces: none\n"
              "    executor: command\n    command:\n      - /bin/true\n"
              "  - name: dup\n    consumes: none\n    produces: none\n"
              "    executor: command\n    command:\n      - /bin/true\n",
              0600);
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) != 0);

   /* an over-long argv element is rejected (no silent truncation) */
   {
      char big[1200];
      memset(big, 'a', sizeof big - 1);
      big[sizeof big - 1] = '\0';
      char body[1600];
      snprintf(body, sizeof body,
               "allow_command: true\nblocks:\n  - name: x\n    consumes: none\n"
               "    produces: none\n    executor: command\n    command:\n      - %s\n",
               big);
      write_file(blocks_path(), body, 0600);
      assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) != 0);
   }

   /* a symlinked registry is refused, not silently treated as "no registry" */
   {
      char target[256], link[256];
      snprintf(target, sizeof target, "%s/workflows/real_blocks.yaml", g_home);
      snprintf(link, sizeof link, "%s/workflows/link_blocks.yaml", g_home);
      write_file(target, GOOD_BLOCKS, 0600);
      assert(symlink(target, link) == 0);
      assert(wfe_custom_registry_load(link, err, sizeof err) != 0);
   }

   /* --- a workflow using a custom block type-checks; a mismatch is rejected --- */
   write_file(blocks_path(), GOOD_BLOCKS, 0600);
   assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) == 0);
   {
      /* impl(branch) -> lint(branch->branch) -> done(merge needs pr) : the lint
       * input must be a branch; feed it a branch and it validates. */
      static const char *WF = "name: cw\nstart: a\nnodes:\n"
                              "  - id: a\n    block: implement\n    in:\n      plan: p.out\n"
                              "    next: lint\n"
                              "  - id: p\n    block: author.plan\n    in:\n      pr: pp.out\n"
                              "  - id: pp\n    block: author.proposal\n    next: a\n"
                              "  - id: lint\n    block: lint\n    in:\n      src: a.out\n";
      /* (graph is illustrative; we only assert the custom node's typing resolves) */
      wfe_def_t *d = wfe_def_parse(WF, err, sizeof err);
      assert(d);
      const wfe_node_t *ln = wfe_def_node(d, "lint");
      assert(ln && ln->block == WFE_BLK_CUSTOM && strcmp(ln->custom_name, "lint") == 0);
      assert(wfe_node_output(ln) == WFE_ART_BRANCH);
      assert(wfe_node_accepts_input(ln, WFE_ART_BRANCH) == 1);
      assert(wfe_node_accepts_input(ln, WFE_ART_PROPOSAL) == 0);
      wfe_def_free(d);
   }

   /* --- exec_custom through the engine: a single `notify` command block
    *     (consumes/produces none, runs `true`) advances to accepted; with
    *     allow_command off the command block fails closed. --- */
   {
      assert(db1_init(":memory:") == 0);
      /* single-node workflow: notify is start + terminal (no edges). */
      static const char *WF = "name: cwf\nstart: n\nnodes:\n"
                              "  - id: n\n    block: notify\n";
      char path[256];
      snprintf(path, sizeof path, "%s/workflows/cwf.yaml", g_home);
      write_file(path, WF, 0644);
      wfe_reset_block_executors();
      wfe_register_default_executors(); /* real exec_custom */

      char id[80] = "";
      assert(wfe_work_item_create("cwf", "r", "pn", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0); /* command ran -> none -> terminal */

      /* allow_command OFF -> the command block fails closed (work item not accepted) */
      write_file(blocks_path(),
                 "blocks:\n  - name: notify\n    consumes: none\n"
                 "    produces: none\n    executor: command\n    command:\n"
                 "      - /bin/true\n",
                 0600);
      assert(wfe_custom_registry_load(blocks_path(), err, sizeof err) == 0);
      assert(wfe_custom_commands_allowed() == 0);
      char id2[80] = "";
      assert(wfe_work_item_create("cwf", "r", "pn2", "interactive", id2, err, sizeof err) == 0);
      assert(wfe_engine_run(id2, err, sizeof err) == 0);
      assert(db1_work_item_get(id2, &wi) == 1);
      assert(strcmp(wi.state, "accepted") != 0); /* failed/parked, not accepted */
   }

   printf("ok\n");
   return 0;
}
