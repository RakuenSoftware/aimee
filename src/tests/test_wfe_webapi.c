/* test_wfe_webapi.c -- /v1/workflow read+author surface (W7). Drives the
 * server_workflow_api.c handlers directly (no HTTP) against a temp $AIMEE_HOME:
 * blocks catalog (built-ins + safety + custom), save/get canonical round-trip
 * byte-stability (incl. a cyclic graph), optimistic-lock conflicts, path-name
 * safety, validate, and work-item run-state. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "db1.h"
#include "server/server_workflow_api.h"
#include "wfe_def.h"
#include "wfe_store.h"

/* Stub the one identity accessor server_workflow_api references (rather than link
 * the full attestation stack). Test-controlled so we can drive both the non-owner
 * (deny) and owner (allow) paths of the ownership check. */
static const char *g_test_principal = "";
const char *server_http_identity_principal(void)
{
   return g_test_principal;
}

#define CAP (64 * 1024)

/* a genuinely valid, cyclic workflow (the gates loop back to pp on failure);
 * typed I/O checks out end-to-end (same shape the engine safety test runs). The
 * params block exercises nested-object + block-sequence canonical round-trip. */
static const char *WF = "name: t1\nstart: pp\nnodes:\n"
                        "  - id: pp\n    block: author.proposal\n    params:\n"
                        "      with_user: true\n    next: pr\n"
                        "  - id: pr\n    block: pr.open\n    in:\n      src: pp.out\n    next: cm\n"
                        "  - id: cm\n    block: check.mergeable\n    in:\n      pr: pr.out\n"
                        "    on_pass: ci\n    on_fail: pp\n"
                        "  - id: ci\n    block: gate.ci\n    in:\n      pr: pr.out\n"
                        "    on_pass: m\n    on_fail: pp\n"
                        "  - id: m\n    block: merge\n    in:\n      pr: pr.out\n";

static int has_block(cJSON *blocks, const char *name, int *custom_out)
{
   cJSON *b = NULL;
   cJSON_ArrayForEach(b, blocks)
   {
      cJSON *jn = cJSON_GetObjectItemCaseSensitive(b, "name");
      if (cJSON_IsString(jn) && strcmp(jn->valuestring, name) == 0)
      {
         if (custom_out)
            *custom_out = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "custom"));
         return 1;
      }
   }
   return 0;
}

static cJSON *parse_resp(const char *buf)
{
   cJSON *o = cJSON_Parse(buf);
   assert(o);
   return o;
}

int main(void)
{
   printf("wfe-webapi: ");
   char home[] = "/tmp/wfe_web_XXXXXX";
   assert(wfe_test_mkdtemp(home));
   char wfdir[128];
   snprintf(wfdir, sizeof wfdir, "%s/workflows", home);
   mkdir(wfdir, 0755);
   char pdir[160];
   snprintf(pdir, sizeof pdir, "%s/workflows/proposals", home);
   mkdir(pdir, 0755);
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);

   char *buf = malloc(CAP);
   assert(buf);

   /* --- blocks catalog: built-ins + safety blocks present --- */
   wfe_custom_registry_reset();
   assert(wf_api_blocks(buf, CAP) == 200);
   {
      cJSON *o = parse_resp(buf);
      cJSON *blocks = cJSON_GetObjectItemCaseSensitive(o, "blocks");
      assert(cJSON_IsArray(blocks));
      assert(has_block(blocks, "author.proposal", NULL));
      assert(has_block(blocks, "gate.ci", NULL));
      assert(has_block(blocks, "check.mergeable", NULL));
      assert(has_block(blocks, "merge", NULL));
      /* manager-loop + sliced-lifecycle blocks (appended after the CUSTOM sentinel)
       * must be modellable in the GUI too. */
      assert(has_block(blocks, "split", NULL));
      assert(has_block(blocks, "branch.open", NULL));
      assert(has_block(blocks, "foreach.workflow", NULL));
      cJSON_Delete(o);
   }

   /* --- custom block surfaces in the catalog --- */
   {
      char p[256];
      snprintf(p, sizeof p, "%s/blocks.yaml", wfdir);
      FILE *f = fopen(p, "wb");
      assert(f);
      fputs("allow_command: true\nblocks:\n  - name: lint\n    consumes: branch\n"
            "    produces: branch\n    executor: command\n    command:\n      - /bin/true\n",
            f);
      fclose(f);
      chmod(p, 0600);
      wfe_custom_registry_reset();
      assert(wf_api_blocks(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      int custom = 0;
      assert(has_block(cJSON_GetObjectItemCaseSensitive(o, "blocks"), "lint", &custom));
      assert(custom == 1);
      cJSON_Delete(o);
   }

   /* --- save a valid (cyclic) workflow --- */
   char v1[80] = "";
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "name", "t1");
      cJSON_AddStringToObject(req, "yaml", WF);
      cJSON_AddStringToObject(req, "prev_version", "");
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      int rc = wf_api_save(body, buf, CAP);
      free(body);
      assert(rc == 200);
      cJSON *o = parse_resp(buf);
      const cJSON *jv = cJSON_GetObjectItemCaseSensitive(o, "version");
      assert(cJSON_IsString(jv) && jv->valuestring[0]);
      snprintf(v1, sizeof v1, "%s", jv->valuestring);
      cJSON_Delete(o);
   }

   /* --- get it back: valid, 3 nodes, canonical present --- */
   char canon[8192] = "";
   {
      assert(wf_api_get("t1", buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "valid")));
      const cJSON *jc = cJSON_GetObjectItemCaseSensitive(o, "canonical");
      assert(cJSON_IsString(jc) && jc->valuestring[0]);
      snprintf(canon, sizeof canon, "%s", jc->valuestring);
      cJSON *def = cJSON_GetObjectItemCaseSensitive(o, "def");
      cJSON *nodes = cJSON_GetObjectItemCaseSensitive(def, "nodes");
      assert(cJSON_GetArraySize(nodes) == 5);
      /* version of the stored def equals the save's reported version */
      const cJSON *jv = cJSON_GetObjectItemCaseSensitive(o, "version");
      assert(strcmp(jv->valuestring, v1) == 0);
      cJSON_Delete(o);
   }

   /* --- byte-stable: canonical(parse(canonical)) == canonical --- */
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "yaml", canon);
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_validate(body, buf, CAP) == 200);
      free(body);
      cJSON *o = parse_resp(buf);
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "valid")));
      const cJSON *jc = cJSON_GetObjectItemCaseSensitive(o, "canonical");
      assert(strcmp(jc->valuestring, canon) == 0); /* idempotent canonical form */
      const cJSON *jv = cJSON_GetObjectItemCaseSensitive(o, "version");
      assert(strcmp(jv->valuestring, v1) == 0); /* stable version */
      cJSON_Delete(o);
   }

   /* --- optimistic lock: correct prev_version succeeds; wrong fails (409) --- */
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "name", "t1");
      cJSON_AddStringToObject(req, "yaml", canon);
      cJSON_AddStringToObject(req, "prev_version", v1);
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_save(body, buf, CAP) == 200); /* match → overwrite ok */
      free(body);
   }
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "name", "t1");
      cJSON_AddStringToObject(req, "yaml", canon);
      cJSON_AddStringToObject(req, "prev_version", "deadbeefdeadbeef");
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_save(body, buf, CAP) == 409); /* mismatch → conflict */
      free(body);
   }
   {
      /* create-when-exists (empty prev) → conflict */
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "name", "t1");
      cJSON_AddStringToObject(req, "yaml", canon);
      cJSON_AddStringToObject(req, "prev_version", "");
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_save(body, buf, CAP) == 409);
      free(body);
   }

   /* --- a present-but-corrupt file blocks a create (empty prev → 409), so it is
    *     never silently overwritten (existence is by stat, not parse success) --- */
   {
      char p[256];
      snprintf(p, sizeof p, "%s/corrupt.yaml", wfdir);
      FILE *f = fopen(p, "wb");
      assert(f);
      fputs("this: is: not: a: valid: workflow: {[\n", f);
      fclose(f);
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "name", "corrupt");
      cJSON_AddStringToObject(req, "yaml", canon);
      cJSON_AddStringToObject(req, "prev_version", "");
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_save(body, buf, CAP) == 409);
      free(body);
   }

   /* --- path-name safety: traversal rejected (400) --- */
   assert(wf_api_get("../etc/passwd", buf, CAP) == 400);
   assert(wf_api_get("a/b", buf, CAP) == 400);
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "name", "../evil");
      cJSON_AddStringToObject(req, "yaml", WF);
      cJSON_AddStringToObject(req, "prev_version", "");
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_save(body, buf, CAP) == 400);
      free(body);
   }

   /* --- get missing → 404 --- */
   assert(wf_api_get("nope", buf, CAP) == 404);

   /* --- invalid definition (unknown block) → valid:false --- */
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "yaml",
                              "name: bad\nstart: a\nnodes:\n  - id: a\n    block: nope\n");
      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      assert(wf_api_validate(body, buf, CAP) == 200);
      free(body);
      cJSON *o = parse_resp(buf);
      assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "valid")));
      cJSON_Delete(o);
   }

   /* --- list includes t1 --- */
   {
      assert(wf_api_list(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      int custom = 0;
      assert(has_block(cJSON_GetObjectItemCaseSensitive(o, "defs"), "t1", &custom));
      cJSON_Delete(o);
   }

   /* --- triggers: none configured (no aimee.yaml) → empty list + default cap --- */
   {
      assert(wf_api_triggers(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      cJSON *trigs = cJSON_GetObjectItemCaseSensitive(o, "triggers");
      assert(cJSON_IsArray(trigs) && cJSON_GetArraySize(trigs) == 0);
      assert(cJSON_HasObjectItem(o, "max_concurrent"));
      cJSON_Delete(o);
   }

   /* --- triggers: a configured watch-dir rule surfaces with its effective
    * defaults (empty event → docs/proposals/pending, empty mode → autonomous). --- */
   {
      char p[256];
      snprintf(p, sizeof p, "%s/aimee.yaml", home);
      FILE *f = fopen(p, "wb");
      assert(f);
      fputs("trigger_rules:\n"
            "  - source: watch-dir\n"
            "    pipeline:\n"
            "      template: build\n"
            "      workspace: /srv/repos/demo\n"
            "  - source: cron\n"
            "    schedule: \"0 * * * *\"\n"
            "    mode: interactive\n"
            "    pipeline:\n"
            "      template: nightly\n",
            f);
      fclose(f);

      assert(wf_api_triggers(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      cJSON *trigs = cJSON_GetObjectItemCaseSensitive(o, "triggers");
      assert(cJSON_IsArray(trigs) && cJSON_GetArraySize(trigs) == 2);
      cJSON *w = cJSON_GetArrayItem(trigs, 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(w, "source")->valuestring, "watch-dir") == 0);
      /* empty event defaults to the proposals dir; empty mode defaults to autonomous */
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(w, "event")->valuestring,
                    "docs/proposals/pending") == 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(w, "mode")->valuestring, "autonomous") == 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(w, "template")->valuestring, "build") == 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(w, "workspace")->valuestring,
                    "/srv/repos/demo") == 0);
      cJSON *c = cJSON_GetArrayItem(trigs, 1);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(c, "source")->valuestring, "cron") == 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(c, "schedule")->valuestring, "0 * * * *") ==
             0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(c, "mode")->valuestring, "interactive") == 0);
      cJSON_Delete(o);
      unlink(p); /* keep the rest of the suite config-free */
   }

   /* --- run-state: empty list + unknown item 404 --- */
   {
      assert(wf_api_items(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(o, "items")));
      cJSON_Delete(o);
   }
   assert(wf_api_item("no-such-item", buf, CAP) == 404);
   assert(wf_api_events("no-such-item", 0, 200, buf, CAP) == 404);
   assert(wf_api_proposal("no-such-item", buf, CAP) == 404);

   /* --- ownership: an INTERACTIVE work item submitted by another principal is not
    * readable --- The test runs with an un-attested (empty) principal, so an
    * interactive item whose submitter is set is owned by nobody-in-this-context:
    * the per-item reads must 403 (the IDOR closure). (Autonomous runs are the
    * deliberate exception — see the operator-visibility block below.) The
    * owner-allowed path + pagination + proposal read-back are exercised live (a
    * request carrying the matching webuser principal). */
   {
      assert(db1_work_item_create("wi_owned", "", "wi_owned.md", "build", "v1", "draft",
                                  "interactive") == 0);
      assert(db1_work_item_set_submitter("wi_owned", "webuser:someone-else") == 0);
      (void)db1_lifecycle_event_add("wi_owned", "draft", "create", "user", "", "", 0.0);

      assert(wf_api_item("wi_owned", buf, CAP) == 403);
      assert(wf_api_events("wi_owned", 0, 200, buf, CAP) == 403);
      assert(wf_api_proposal("wi_owned", buf, CAP) == 403);

      /* The owner-scoped list must not leak the non-owned item... */
      assert(wf_api_items(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      cJSON *items = cJSON_GetObjectItemCaseSensitive(o, "items");
      assert(cJSON_IsArray(items) && cJSON_GetArraySize(items) == 0);
      cJSON_Delete(o);

      /* ...but the operator "all" view returns it, enriched with the new fields. */
      assert(wf_api_items_all(buf, CAP) == 200);
      o = parse_resp(buf);
      items = cJSON_GetObjectItemCaseSensitive(o, "items");
      assert(cJSON_IsArray(items) && cJSON_GetArraySize(items) == 1);
      cJSON *it = cJSON_GetArrayItem(items, 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(it, "submitter")->valuestring,
                    "webuser:someone-else") == 0);
      assert(strcmp(cJSON_GetObjectItemCaseSensitive(it, "proposal_name")->valuestring,
                    "wi_owned.md") == 0);
      assert(cJSON_HasObjectItem(it, "cum_cost_usd") && cJSON_HasObjectItem(it, "pr_ref"));
      cJSON_Delete(o);
   }

   /* --- autonomous / triggered runs are OPERATOR-VISIBLE regardless of submitter ---
    * A system-initiated run (mode "autonomous": the proposals trigger, cron, or a
    * dev-submit pipeline) is not a private human proposal, so a dashboard operator
    * — here the un-attested (non-submitting) principal — can read it AND it appears
    * in the DEFAULT owner-scoped list. This is what surfaces the autonomous pipeline
    * in the Workflows tab. Interactive proposals stay owner-scoped (block above). */
   {
      assert(db1_work_item_create("wi_auto", "", "wi_auto.md", "build", "v1", "draft",
                                  "autonomous") == 0);
      assert(db1_work_item_set_submitter("wi_auto", "webuser:someone-else") == 0);
      (void)db1_lifecycle_event_add("wi_auto", "draft", "create", "engine", "", "", 0.0);

      /* principal is empty (non-owner), yet the autonomous run is readable... */
      assert(wf_api_item("wi_auto", buf, CAP) == 200);
      assert(wf_api_events("wi_auto", 0, 200, buf, CAP) == 200);
      /* ...and it shows in the default owner-scoped list (the Workflows tab). */
      assert(wf_api_items(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      cJSON *items = cJSON_GetObjectItemCaseSensitive(o, "items");
      int found = 0;
      for (int i = 0; i < cJSON_GetArraySize(items); i++)
      {
         cJSON *it = cJSON_GetArrayItem(items, i);
         if (strcmp(cJSON_GetObjectItemCaseSensitive(it, "id")->valuestring, "wi_auto") == 0)
            found = 1;
      }
      assert(found);
      cJSON_Delete(o);
   }

   /* --- owner-allowed path: events pagination + proposal read-back + symlink guard.
    * Drive the stub principal to match the item's submitter so wf_owns permits. --- */
   {
      g_test_principal = "webuser:someone-else";

      /* Proposal read-back: write the source file the item references. */
      char pfile[256];
      snprintf(pfile, sizeof pfile, "%s/wi_owned.md", pdir);
      FILE *pf = fopen(pfile, "wb");
      assert(pf);
      fputs("# My proposal\n\nbody\n", pf);
      fclose(pf);
      assert(wf_api_proposal("wi_owned", buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      assert(
          strstr(cJSON_GetObjectItemCaseSensitive(o, "proposal_md")->valuestring, "My proposal"));
      assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "truncated")));
      cJSON_Delete(o);

      assert(wf_api_item("wi_owned", buf, CAP) == 200); /* owner sees it now */

      /* Events pagination: add three more events (id-ascending), page by 2. */
      (void)db1_lifecycle_event_add("wi_owned", "draft", "advance", "engine", "", "", 0.0);
      (void)db1_lifecycle_event_add("wi_owned", "impl", "advance", "engine", "", "", 0.0);
      (void)db1_lifecycle_event_add("wi_owned", "impl", "pause", "engine", "pending_human", "",
                                    0.0);
      assert(wf_api_events("wi_owned", 0, 2, buf, CAP) == 200);
      o = parse_resp(buf);
      cJSON *evs = cJSON_GetObjectItemCaseSensitive(o, "events");
      assert(cJSON_IsArray(evs) && cJSON_GetArraySize(evs) == 2);
      long next_after = (long)cJSON_GetObjectItemCaseSensitive(o, "next_after")->valuedouble;
      long last_id =
          (long)cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(evs, 1), "id")->valuedouble;
      assert(next_after == last_id); /* cursor = id of the LAST returned event */
      cJSON_Delete(o);
      /* Next page continues past the cursor with no gap/dup (4 events total). */
      assert(wf_api_events("wi_owned", next_after, 200, buf, CAP) == 200);
      o = parse_resp(buf);
      evs = cJSON_GetObjectItemCaseSensitive(o, "events");
      assert(cJSON_GetArraySize(evs) == 2);
      assert((long)cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(evs, 0), "id")->valuedouble >
             next_after);
      cJSON_Delete(o);

      /* Symlink guard: an item whose proposal file is a symlink (even to a readable
       * file) is refused by openat(O_NOFOLLOW) → 403, never followed. */
      char secret[256], link[256];
      snprintf(secret, sizeof secret, "%s/secret.txt", home);
      FILE *sf = fopen(secret, "wb");
      assert(sf);
      fputs("TOP SECRET\n", sf);
      fclose(sf);
      snprintf(link, sizeof link, "%s/wi_link.md", pdir);
      assert(symlink(secret, link) == 0);
      assert(db1_work_item_create("wi_link", "", "wi_link.md", "build", "v1", "draft",
                                  "autonomous") == 0);
      assert(db1_work_item_set_submitter("wi_link", "webuser:someone-else") == 0);
      assert(wf_api_proposal("wi_link", buf, CAP) == 403); /* symlink not followed */

      g_test_principal = "";
   }

   /* --- custom delegate-block CRUD: write blocks.yaml + reload round-trip, and
    * preserve an existing operator command block untouched --- */
   {
      char p[256];
      snprintf(p, sizeof p, "%s/blocks.yaml", wfdir);
      FILE *f = fopen(p, "wb");
      assert(f);
      fputs("allow_command: true\nblocks:\n  - name: lintcmd\n    consumes: branch\n"
            "    produces: branch\n    executor: command\n    command:\n      - /bin/true\n",
            f);
      fclose(f);
      chmod(p, 0600);
      wfe_custom_registry_reset();

      const char *body = "{\"consumes\":\"proposal\",\"produces\":\"branch\","
                         "\"persona\":\"architect\",\"prompt\":\"Refine the proposal.\"}";
      assert(wf_api_block_put("refine", body, buf, CAP) == 200);
      assert(wf_api_blocks(buf, CAP) == 200);
      {
         cJSON *o = parse_resp(buf);
         cJSON *blocks = cJSON_GetObjectItemCaseSensitive(o, "blocks");
         cJSON *b = NULL, *found = NULL;
         cJSON_ArrayForEach(b, blocks)
         {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(b, "name");
            if (cJSON_IsString(n) && strcmp(n->valuestring, "refine") == 0)
               found = b;
         }
         assert(found);
         assert(strcmp(cJSON_GetObjectItemCaseSensitive(found, "executor")->valuestring,
                       "delegate") == 0);
         assert(strcmp(cJSON_GetObjectItemCaseSensitive(found, "persona")->valuestring,
                       "architect") == 0);
         /* the operator's command block survived the write */
         assert(has_block(blocks, "lintcmd", NULL));
         cJSON_Delete(o);
      }
      /* validation: shadow a built-in → 409, missing persona/prompt → 400, bad name → 400 */
      assert(wf_api_block_put("merge", body, buf, CAP) == 409);
      assert(wf_api_block_put("bad", "{\"consumes\":\"none\"}", buf, CAP) == 400);
      assert(wf_api_block_put("a/b", body, buf, CAP) == 400);
      /* a UI delete of the command block is refused (operator-managed) */
      assert(wf_api_block_delete("lintcmd", buf, CAP) == 403);
      /* delete the delegate block */
      assert(wf_api_block_delete("refine", buf, CAP) == 200);
      assert(wf_api_blocks(buf, CAP) == 200);
      {
         cJSON *o = parse_resp(buf);
         assert(!has_block(cJSON_GetObjectItemCaseSensitive(o, "blocks"), "refine", NULL));
         assert(has_block(cJSON_GetObjectItemCaseSensitive(o, "blocks"), "lintcmd", NULL));
         cJSON_Delete(o);
      }
   }

   free(buf);
   printf("ok\n");
   return 0;
}
