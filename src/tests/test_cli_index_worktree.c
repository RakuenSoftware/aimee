/* A remote service cannot inspect a client's temporary linked checkout. */
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "cli_v1_routes_internal.h"
#include "platform_test_util.h"
#include "util.h"

static void run(const char *const argv[])
{
   char *output = NULL;
   int rc = safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, &output, 16384, 10000);
   if (rc != 0)
      fprintf(stderr, "%s: %s\n", argv[0], output ? output : "no output");
   free(output);
   assert(rc == 0);
}

static cJSON *request(const char *cwd)
{
   cJSON *req = cJSON_CreateObject();
   assert(req);
   cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

static void add_project(cJSON *response, const char *name, const char *root)
{
   cJSON *project = cJSON_CreateObject();
   cJSON_AddStringToObject(project, "name", name);
   cJSON_AddStringToObject(project, "root", root);
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(response, "projects"), project);
}

static void check_project(const char *cwd, const char *main_root, cJSON *response,
                          const char *expected)
{
   cJSON *req = request(cwd);
   cli_index_apply_worktree_project(req, main_root, response);
   const char *actual = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "project"));
   assert(expected ? actual && strcmp(actual, expected) == 0 : actual == NULL);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "cwd")), cwd) == 0);
   cJSON_Delete(req);
}

static void check_forwarding(const char *main_root, const char *cwd)
{
   char original[4096];
   assert(getcwd(original, sizeof(original)) && chdir(cwd) == 0);
   unsetenv("AIMEE_PROJECT_ID");
   cJSON *registry = cJSON_Parse("{\"projects\":[]}");
   add_project(registry, "registered-project", main_root);
   char *registry_json = cJSON_PrintUnformatted(registry);
   cJSON_Delete(registry);
   for (int scenario = 0; scenario < 7; scenario++)
   {
      int with_registry = scenario < 3 || scenario == 6;
      const char *expected = scenario == 2 || scenario == 4 ? NULL
                             : scenario == 3                ? "explicit"
                             : scenario == 5                ? "launcher-project"
                                                            : "registered-project";
      int listener = socket(AF_INET, SOCK_STREAM, 0);
      assert(listener >= 0);
      struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
      assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
      assert(listen(listener, 4) == 0);
      socklen_t size = sizeof(addr);
      assert(getsockname(listener, (struct sockaddr *)&addr, &size) == 0);
      char endpoint[64];
      snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d", ntohs(addr.sin_port));
      setenv("AIMEE_API_ENDPOINT", endpoint, 1);
      cJSON *manifest =
          cJSON_Parse("{\"manifest_version\":1,\"routes\":["
                      "{\"op\":\"index.list\",\"verb\":\"GET\",\"path\":\"/v1/index/projects\"},"
                      "{\"op\":\"index.hybrid\",\"verb\":\"POST\",\"path\":\"/v1/index/query\"},"
                      "{\"op\":\"kb.search\",\"verb\":\"POST\",\"path\":\"/v1/index/query\"}]}");
      if (scenario == 1)
         cJSON_AddItemToObject(
             manifest, "marshal",
             cJSON_Parse("[{\"method\":\"index.hybrid\",\"args\":{\"fields\":["
                         "{\"json\":\"query\",\"from\":\"positional\",\"index\":0},"
                         "{\"json\":\"cwd\",\"from\":\"cwd\"}]}}]"));
      cli_v1_manifest_set_for_test(manifest);
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         alarm(10);
         for (int i = 0; i < 1 + with_registry; i++)
         {
            int fd = accept(listener, NULL, NULL);
            assert(fd >= 0);
            char bytes[16384] = "";
            size_t used = 0;
            char *body = NULL;
            for (;;)
            {
               ssize_t n = recv(fd, bytes + used, sizeof(bytes) - used - 1, 0);
               assert(n > 0);
               used += (size_t)n;
               bytes[used] = '\0';
               char *end = strstr(bytes, "\r\n\r\n");
               char *length = strstr(bytes, "Content-Length:");
               if (end && length &&
                   used >= (size_t)(end + 4 - bytes) + strtoul(length + 15, NULL, 10))
               {
                  body = end + 4;
                  break;
               }
               assert(used < sizeof(bytes) - 1);
            }
            int is_registry = with_registry && i == 0;
            if (is_registry)
               assert(strstr(bytes, " /v1/index/projects "));
            else
            {
               assert(strstr(bytes, scenario == 6 ? " /v1/kb/search " : " /v1/index/query "));
               cJSON *req = cJSON_Parse(body);
               const char *project =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "project"));
               assert(expected ? project && strcmp(project, expected) == 0 : project == NULL);
               assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "cwd")),
                             cwd) == 0);
               cJSON_Delete(req);
            }
            const char *response =
                is_registry ? registry_json
                            : "{\"status\":\"ok\",\"results\":[{\"result\":{\"hits\":[]}}]}";
            char header[256];
            int n = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d Fixture\r\nContent-Type: application/json\r\n"
                             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                             is_registry && scenario == 2 ? 503 : 200, strlen(response));
            assert(send(fd, header, (size_t)n, 0) == n);
            assert(send(fd, response, strlen(response), 0) == (ssize_t)strlen(response));
            close(fd);
         }
         close(listener);
         _exit(0);
      }
      close(listener);
      cli_v1_route_t route = {.method = scenario == 6 ? "kb.search" : "index.hybrid",
                              .timeout_ms = 2000};
      char *args[] = {"needle", scenario == 4 ? "--scope" : "--project",
                      scenario == 4 ? "all" : "explicit"};
      if (scenario == 5)
         setenv("AIMEE_PROJECT_ID", "launcher-project", 1);
      assert(cli_v1_forward(NULL, &route, 1, NULL, NULL, scenario == 3 || scenario == 4 ? 3 : 1,
                            args) == 0);
      unsetenv("AIMEE_PROJECT_ID");
      int status = 0;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   free(registry_json);
   cli_v1_manifest_set_for_test(NULL);
   unsetenv("AIMEE_API_ENDPOINT");
   assert(chdir(original) == 0);
}

int main(void)
{
   char tmp[4096], main_root[4096], linked[4096], subdir[4096];
   snprintf(tmp, sizeof(tmp), "%s/aimee-index-worktree-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmp));
   assert(snprintf(main_root, sizeof(main_root), "%s/main repo", tmp) < (int)sizeof(main_root));
   assert(snprintf(linked, sizeof(linked), "%s/work tree ;$`quoted`", tmp) < (int)sizeof(linked));
   assert(snprintf(subdir, sizeof(subdir), "%s/sub", linked) < (int)sizeof(subdir));
   run((const char *const[]){"git", "init", "-q", main_root, NULL});
   run((const char *const[]){"git", "-C", main_root, "-c", "user.name=test", "-c",
                             "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                             "commit", "--allow-empty", "-qm", "fixture", NULL});
   run((const char *const[]){"git", "-C", main_root, "worktree", "add", "-q", "--detach", linked,
                             "HEAD", NULL});
   assert(mkdir(subdir, 0700) == 0);

   cJSON *req = request(subdir);
   const char *methods[] = {"index.find",
                            "index.structure",
                            "index.blast_radius",
                            "index.span",
                            "index.hybrid",
                            "index.investigate",
                            "index.find_callers",
                            "index.find_callees",
                            "index.deps",
                            "kb.search",
                            NULL};
   for (int i = 0; methods[i]; i++)
   {
      char *root = cli_index_worktree_root(methods[i], req);
      assert(root && strcmp(root, main_root) == 0);
      free(root);
   }
   assert(cli_index_worktree_root("memory.search", req) == NULL);
   assert(cli_index_worktree_root("index.scan", req) == NULL);
   cJSON_AddStringToObject(req, "scope", "all");
   assert(cli_index_worktree_root("index.hybrid", req) == NULL);
   cJSON_DeleteItemFromObjectCaseSensitive(req, "scope");
   /* Explicit flags, historic positionals and launcher environment values all
    * arrive as project by this stage, through native and argspec marshallers. */
   cJSON_AddStringToObject(req, "project", "explicit");
   assert(cli_index_worktree_root("index.hybrid", req) == NULL);
   cJSON *response = cJSON_Parse("{\"projects\":[]}");
   add_project(response, "registered-name-not-a-basename", main_root);
   cli_index_apply_worktree_project(req, main_root, response);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "project")),
                 "explicit") == 0);
   cJSON_Delete(req);
   req = request(main_root);
   assert(cli_index_worktree_root("index.hybrid", req) == NULL);
   cJSON_Delete(req);
   req = request(tmp); /* Outside any repository: preserve the server fallback. */
   assert(cli_index_worktree_root("index.hybrid", req) == NULL);
   cJSON_Delete(req);

   check_project(subdir, main_root, response, "registered-name-not-a-basename");
   check_project(subdir, "/unregistered", response, NULL);
   check_project(subdir, main_root, NULL, NULL);
   cJSON *error = cJSON_Parse("{\"status\":\"error\"}");
   check_project(subdir, main_root, error, NULL);
   cJSON_Delete(error);
   char sibling[4096];
   assert(snprintf(sibling, sizeof(sibling), "%s-other", main_root) < (int)sizeof(sibling));
   check_project(subdir, sibling, response, NULL); /* Component boundary. */
   add_project(response, "another-name", main_root);
   check_project(subdir, main_root, response, NULL); /* Ambiguous, independent of order. */
   add_project(response, "registered-name-not-a-basename", main_root);
   check_project(subdir, main_root, response, NULL);
   add_project(response, "worktree-project", linked);
   check_project(subdir, main_root, response, "worktree-project");
   add_project(response, "nested-project", subdir);
   check_project(subdir, main_root, response, "nested-project");
   add_project(response, "nested-alias", subdir);
   check_project(subdir, main_root, response, NULL);
   cJSON_Delete(response);
   response = cJSON_Parse("{\"projects\":[]}");
   char trailing[4096];
   assert(snprintf(trailing, sizeof(trailing), "%s/", main_root) < (int)sizeof(trailing));
   add_project(response, "registered", trailing);
   check_project(subdir, main_root, response, "registered");
   cJSON_Delete(response);

   check_forwarding(main_root, subdir);
   run((const char *const[]){"git", "-C", main_root, "worktree", "remove", "--force", linked,
                             NULL});
   run((const char *const[]){"rm", "-rf", "--", tmp, NULL});
   puts("PASS: linked-worktree project inference and registered-scope precedence");
   return 0;
}
