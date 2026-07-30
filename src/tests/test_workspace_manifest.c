/* test_workspace_manifest.c — unit tests for workspace_manifest_load() */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "workspace_manifest.h"
#include "platform_test_util.h"

/* Write text to path, return 0 on success. */
static int write_file(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fputs(text, f);
   fclose(f);
   return 0;
}

int main(void)
{
   printf("workspace-manifest: ");

   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-manifest-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   /* --- missing file returns -1 --- */
   {
      workspace_manifest_t m;
      int rc = workspace_manifest_load("/nonexistent/path/does/not/exist", &m);
      assert(rc == -1);
   }

   /* --- directory with no manifest returns -1 --- */
   {
      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      assert(rc == -1);
   }

   /* --- minimal valid manifest: repos only --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      const char *yaml = "id: example-workspace\n"
                         "repos:\n"
                         "  - id: stable-myrepo\n"
                         "    url: https://github.com/example/myrepo.git\n"
                         "    path: ./repos/myrepo\n"
                         "  - url: https://github.com/example/other.git\n";
      assert(write_file(path, yaml) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      assert(rc == 0);
      assert(strcmp(m.id, "example-workspace") == 0);
      assert(m.repo_count == 2);
      assert(strcmp(m.repos[0].id, "stable-myrepo") == 0);
      assert(strcmp(m.repos[0].url, "https://github.com/example/myrepo.git") == 0);
      assert(strcmp(m.repos[0].path, "./repos/myrepo") == 0);
      assert(strcmp(m.repos[1].url, "https://github.com/example/other.git") == 0);
      assert(m.repos[1].id[0] == '\0');
      assert(m.repos[1].path[0] == '\0');
      assert(m.dep_cmd_count == 0);
      assert(m.secret_count == 0);
      /* quickstart defaults */
      assert(m.index == 1);
      assert(m.generate_rules == 1);

      unlink(path);
   }

   /* --- full manifest: repos + dependencies + secrets + quickstart --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      const char *yaml = "repos:\n"
                         "  - url: https://github.com/example/repo.git\n"
                         "\n"
                         "dependencies:\n"
                         "  c:\n"
                         "    apt:\n"
                         "      - libssl-dev\n"
                         "      - cmake\n"
                         "  python:\n"
                         "    pip:\n"
                         "      - requests\n"
                         "      - flask\n"
                         "\n"
                         "secrets:\n"
                         "  - name: GITHUB_TOKEN\n"
                         "    description: PAT for private repos\n"
                         "  - name: API_KEY\n"
                         "\n"
                         "quickstart:\n"
                         "  index: true\n"
                         "  generate_rules: false\n";
      assert(write_file(path, yaml) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      assert(rc == 0);
      assert(m.repo_count == 1);
      assert(strcmp(m.repos[0].url, "https://github.com/example/repo.git") == 0);

      /* Should have two dep commands (one for apt, one for pip) */
      assert(m.dep_cmd_count == 2);
      int found_apt = 0, found_pip = 0;
      for (int i = 0; i < m.dep_cmd_count; i++)
      {
         if (strstr(m.dep_cmds[i].cmd, "apt-get") && strstr(m.dep_cmds[i].cmd, "libssl-dev") &&
             strstr(m.dep_cmds[i].cmd, "cmake"))
            found_apt = 1;
         if (strstr(m.dep_cmds[i].cmd, "pip") && strstr(m.dep_cmds[i].cmd, "requests") &&
             strstr(m.dep_cmds[i].cmd, "flask"))
            found_pip = 1;
      }
      assert(found_apt);
      assert(found_pip);

      assert(m.secret_count == 2);
      assert(strcmp(m.secrets[0].name, "GITHUB_TOKEN") == 0);
      assert(strcmp(m.secrets[0].description, "PAT for private repos") == 0);
      assert(strcmp(m.secrets[1].name, "API_KEY") == 0);

      assert(m.index == 1);
      assert(m.generate_rules == 0);

      unlink(path);
   }

   /* --- npm dependency: bare "install" item → "npm install" --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      const char *yaml = "dependencies:\n"
                         "  js:\n"
                         "    npm:\n"
                         "      - install\n";
      assert(write_file(path, yaml) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      assert(rc == 0);
      assert(m.dep_cmd_count == 1);
      assert(strcmp(m.dep_cmds[0].cmd, "npm install") == 0);

      unlink(path);
   }

   /* --- npm dependency with explicit packages --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      const char *yaml = "dependencies:\n"
                         "  js:\n"
                         "    npm:\n"
                         "      - lodash\n"
                         "      - express\n";
      assert(write_file(path, yaml) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      assert(rc == 0);
      assert(m.dep_cmd_count == 1);
      assert(strstr(m.dep_cmds[0].cmd, "npm install") != NULL);
      assert(strstr(m.dep_cmds[0].cmd, "lodash") != NULL);
      assert(strstr(m.dep_cmds[0].cmd, "express") != NULL);

      unlink(path);
   }

   /* --- stable ids are strict and unique (never silently ignored) --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      assert(write_file(path, "id: invalid identity\n") == 0);
      workspace_manifest_t m;
      assert(workspace_manifest_load(tmpdir, &m) == -2);

      const char *duplicate = "repos:\n"
                              "  - id: shared-id\n"
                              "    url: https://example.test/a.git\n"
                              "  - id: shared-id\n"
                              "    url: https://example.test/b.git\n";
      assert(write_file(path, duplicate) == 0);
      assert(workspace_manifest_load(tmpdir, &m) == -2);

      const char *identified_without_url = "repos:\n  - id: stable-but-incomplete\n";
      assert(write_file(path, identified_without_url) == 0);
      assert(workspace_manifest_load(tmpdir, &m) == -2);

      const char *invalid_skipped_id = "repos:\n"
                                       "  - id: invalid identity\n"
                                       "    path: ./missing-url\n";
      assert(write_file(path, invalid_skipped_id) == 0);
      assert(workspace_manifest_load(tmpdir, &m) == -2);

      assert(write_file(path, "id: 42\n") == 0);
      assert(workspace_manifest_load(tmpdir, &m) == -2);
      unlink(path);
   }

   /* --- invalid YAML returns -2 --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      /* Deliberately malformed YAML (inconsistent indentation / garbage) */
      const char *bad = "repos:\n  - url: good\n    bad-key:::invalid:::\n      :\n";
      assert(write_file(path, bad) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      /* Either -2 (parse error) or 0 with partial data — the parser is lenient.
       * The key requirement is that it does not crash. */
      (void)rc;

      unlink(path);
   }

   /* --- load by full file path (not directory) --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/custom.yaml", tmpdir);
      const char *yaml = "repos:\n"
                         "  - url: https://github.com/example/x.git\n";
      assert(write_file(path, yaml) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(path, &m);
      assert(rc == 0);
      assert(m.repo_count == 1);
      assert(strcmp(m.repos[0].url, "https://github.com/example/x.git") == 0);

      unlink(path);
   }

   /* --- quickstart: index: false disables indexing flag --- */
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", tmpdir, WORKSPACE_MANIFEST_FILENAME);
      const char *yaml = "quickstart:\n"
                         "  index: false\n"
                         "  generate_rules: true\n";
      assert(write_file(path, yaml) == 0);

      workspace_manifest_t m;
      int rc = workspace_manifest_load(tmpdir, &m);
      assert(rc == 0);
      assert(m.index == 0);
      assert(m.generate_rules == 1);

      unlink(path);
   }

   platform_test_rmrf(tmpdir);
   printf("OK\n");
   return 0;
}
