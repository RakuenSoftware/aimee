/* test_workspace_handle.c: the /v1 workspace manifest builder shapes the
 * handle response (workspace-resource-plane §1). */
#include "modules/workspace/workspace_handle.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *sget(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

int main(void)
{
   /* --- a git-backed workspace --- */
   {
      cJSON *m = workspace_manifest_json("/home/u/proj", "shared", 1, 1, "git@github.com:o/r.git",
                                         "abc123", "main");
      assert(m != NULL);
      assert(strcmp(sget(m, "root"), "/home/u/proj") == 0);
      assert(strcmp(sget(m, "provider"), "shared") == 0);
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "exists")));
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "is_dir")));
      cJSON *vcs = cJSON_GetObjectItemCaseSensitive(m, "vcs");
      assert(cJSON_IsObject(vcs));
      assert(strcmp(sget(vcs, "remote"), "git@github.com:o/r.git") == 0);
      assert(strcmp(sget(vcs, "head"), "abc123") == 0);
      assert(strcmp(sget(vcs, "branch"), "main") == 0);
      cJSON_Delete(m);
   }

   /* --- a non-existent path / non-repo: booleans false, vcs empty, NULLs -> "" --- */
   {
      cJSON *m = workspace_manifest_json("/nope", "shared", 0, 0, NULL, NULL, NULL);
      assert(m != NULL);
      assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(m, "exists")));
      assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(m, "is_dir")));
      cJSON *vcs = cJSON_GetObjectItemCaseSensitive(m, "vcs");
      assert(strcmp(sget(vcs, "remote"), "") == 0);
      assert(strcmp(sget(vcs, "head"), "") == 0);
      assert(strcmp(sget(vcs, "branch"), "") == 0);
      cJSON_Delete(m);
   }

   /* --- NULL root/provider default safely --- */
   {
      cJSON *m = workspace_manifest_json(NULL, NULL, 1, 0, "", "", "");
      assert(m != NULL);
      assert(strcmp(sget(m, "root"), "") == 0);
      assert(strcmp(sget(m, "provider"), "shared") == 0);
      cJSON_Delete(m);
   }

   printf("workspace_handle: all tests passed\n");
   return 0;
}
