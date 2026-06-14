/* test_wfe_webapi.c -- /v1/workflow read+author surface (W7). Drives the
 * server_workflow_api.c handlers directly (no HTTP) against a temp $AIMEE_HOME:
 * blocks catalog (built-ins + safety + custom), save/get canonical round-trip
 * byte-stability (incl. a cyclic graph), optimistic-lock conflicts, path-name
 * safety, validate, and work-item run-state. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "db1.h"
#include "server/server_workflow_api.h"
#include "wfe_def.h"

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
   assert(mkdtemp(home));
   char wfdir[128];
   snprintf(wfdir, sizeof wfdir, "%s/workflows", home);
   mkdir(wfdir, 0755);
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

   /* --- run-state: empty list + unknown item 404 --- */
   {
      assert(wf_api_items(buf, CAP) == 200);
      cJSON *o = parse_resp(buf);
      assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(o, "items")));
      cJSON_Delete(o);
   }
   assert(wf_api_item("no-such-item", buf, CAP) == 404);

   free(buf);
   printf("ok\n");
   return 0;
}
