/* Exercise the real controller and its joined subprocess without a Docker
 * daemon. Only external authority owners and socket discovery are substituted;
 * the helper below speaks the actual bounded receipt contract over stdio. */
#include <assert.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h"

static int socket_available = 1;
static int fixture_stat(const char *path, struct stat *out)
{
   if (!strcmp(path, "/var/run/docker.sock"))
   {
      memset(out, 0, sizeof *out);
      out->st_mode = S_IFSOCK | 0600;
      return socket_available ? 0 : -1;
   }
   return stat(path, out);
}
#define stat(...) fixture_stat(__VA_ARGS__)
#include "../server/server_verification.c"
#undef stat

static request_context_t context;
static verify_execution_begin_fn registered;
static verify_config_t fixture_config;
static char source_root[4096];
static int capture_refs, mirror_refs, snapshots, releases;
static int source_clean = 1, snapshot_clean = 1, release_ok = 1, observer_ok = 1;
static int accepting = 1, configured_image = 1;
static server_session_terminal_action_fn observer;
static server_session_verification_prepare_fn observer_prepare;
static void *observer_resource;
static const char commit_id[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char tree_id[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char lease_id[] = "cccccccccccccccccccccccccccccccc";
static const char cursor_id[] = "9007199254740993";
static server_session_capture_t *capture = (server_session_capture_t *)&context;

const char *aimee_home(void)
{
   return source_root;
}
const request_context_t *request_context_get(void)
{
   return &context;
}
void verify_register_execution_owner(verify_execution_begin_fn callback)
{
   registered = callback;
}
int verify_load_config(const char *root, verify_config_t *out)
{
   assert(!strcmp(root, source_root));
   *out = fixture_config;
   return 0;
}
char *verify_compute_commit_hash(const char *root)
{
   assert(!strncmp(root, "/proc/", 6));
   return strdup(commit_id);
}
char *verify_compute_file_hash(const char *root)
{
   assert(!strncmp(root, "/proc/", 6));
   return strdup(tree_id);
}
int workspace_turn_resource_identity(const char *root, int *fd, char digest[65])
{
   assert(!strcmp(root, source_root));
   *fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   memset(digest, 'd', 64);
   digest[64] = 0;
   return *fd < 0 ? -1 : 0;
}
int workspace_turn_mirror_capture(const char *root, workspace_turn_mirror_lease_t **out)
{
   assert(!strcmp(root, source_root));
   *out = (workspace_turn_mirror_lease_t *)&fixture_config;
   mirror_refs++;
   return 0;
}
void workspace_turn_mirror_release(workspace_turn_mirror_lease_t *resource)
{
   if (resource)
   {
      assert(resource == (workspace_turn_mirror_lease_t *)&fixture_config);
      mirror_refs--;
   }
}
int server_learning_project_from_cwd(const char *root, char *out, size_t cap)
{
   assert(!strcmp(root, source_root));
   snprintf(out, cap, "project-one");
   return 0;
}
int delegate_sandbox_resolve_image(const char *root, char *out, size_t cap)
{
   assert(!strcmp(root, source_root));
   snprintf(out, cap, "%s", configured_image ? "fixture:toolchain" : "");
   return configured_image ? 0 : -1;
}
const char *delegate_backend_docker_default_image(void)
{
   return "ubuntu:22.04";
}
int server_session_capture_retain_current(const char *session, const char *actor,
                                          server_session_capture_t **out)
{
   assert(!strcmp(session, "execution-source") && !strcmp(actor, "alice"));
   *out = capture;
   capture_refs++;
   return 0;
}
const char *server_session_capture_session(const server_session_capture_t *owner)
{
   assert(owner == capture);
   return "canonical-session";
}
const char *server_session_capture_turn(const server_session_capture_t *owner)
{
   assert(owner == capture);
   return "canonical-turn";
}
void server_session_capture_release(server_session_capture_t *owner)
{
   if (owner)
   {
      assert(owner == capture);
      capture_refs--;
   }
}
int server_session_capture_accepting(const server_session_capture_t *owner)
{
   assert(owner == capture);
   return accepting;
}
int server_session_capture_cursor(server_session_capture_t *owner, char out[32])
{
   assert(owner == capture);
   snprintf(out, 32, "%s", cursor_id);
   return 0;
}
int server_session_capture_observe_verification(server_session_capture_t *owner,
                                                server_session_verification_prepare_fn prepare,
                                                server_session_terminal_action_fn action,
                                                void *resource)
{
   assert(owner == capture && !observer && prepare);
   if (!observer_ok)
      return -1;
   observer = action;
   observer_prepare = prepare;
   observer_resource = resource;
   return 0;
}
cJSON *aimee_module_json_call(uint32_t event, uint32_t stage, cJSON *request, size_t cap,
                              int timeout, aimee_module_call_result_t *result)
{
   assert(event == AIMEE_GIT_EVENT_VERIFY_RUN && stage == AIMEE_GIT_STAGE_VERIFY_RUN);
   assert(cap >= 16384 && timeout > 0 && !result);
   const char *op = field(request, "op");
   cJSON *reply = cJSON_CreateObject();
   int ok = 1;
   if (!strcmp(op, "snapshot-source-state"))
   {
      assert(!strcmp(field(request, "project_root"), source_root));
      assert(!strcmp(field(request, "commit"), commit_id));
      assert(!strcmp(field(request, "tree"), tree_id));
      cJSON_AddBoolToObject(reply, "clean", source_clean);
   }
   else if (!strcmp(op, "snapshot-create"))
   {
      snapshots++;
      cJSON_AddStringToObject(reply, "lease", lease_id);
      cJSON_AddStringToObject(reply, "path", source_root);
   }
   else
   {
      assert(!strcmp(field(request, "lease"), lease_id));
      if (!strcmp(op, "snapshot-release"))
      {
         releases++;
         ok = release_ok;
      }
      else
      {
         assert(!strcmp(op, "snapshot-state") || !strcmp(op, "snapshot-renew"));
         cJSON_AddBoolToObject(reply, "clean", snapshot_clean);
      }
   }
   cJSON_AddStringToObject(reply, "commit", commit_id);
   cJSON_AddStringToObject(reply, "tree", tree_id);
   cJSON_AddBoolToObject(reply, "ok", ok);
   cJSON_Delete(request);
   return reply;
}
static int helper(int argc, char **argv)
{
   int parent_seen = 0, owner_seen = 0;
   for (int i = 2; i < argc; ++i)
      if (!strcmp(argv[i], "--owner-state-dir"))
      {
         assert(!owner_seen && i + 1 < argc && argv[i + 1][0] == '/');
         assert(strstr(argv[++i], "/verification-workers"));
         owner_seen = 1;
      }
      else if (!strcmp(argv[i], "--parent-pid"))
      {
         assert(!parent_seen && i + 1 < argc);
         char *end;
         errno = 0;
         long parent = strtol(argv[++i], &end, 10);
         assert(!errno && end != argv[i] && !*end && parent > 0 && parent == (long)getppid());
         parent_seen = 1;
      }
   assert(parent_seen && owner_seen);

   char buffer[32768];
   size_t length = fread(buffer, 1, sizeof buffer - 1, stdin);
   buffer[length] = 0;
   cJSON *request = cJSON_ParseWithOpts(buffer, NULL, 1);
   assert(request && !strcmp(field(request, "schema"), "aimee.private-tests.request.v1"));
   assert(!strcmp(field(request, "snapshot_id"), lease_id));
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(request, "steps");
   const char *mode = field(cJSON_GetArrayItem(steps, 0), "run");
   if (!strcmp(mode, "slow"))
      sleep(3);
   if (!strcmp(mode, "malformed"))
   {
      cJSON_Delete(request);
      fputs("{not JSON}", stdout);
      return 0;
   }
   cJSON *receipt = cJSON_CreateObject();
   cJSON_AddStringToObject(receipt, "schema", "aimee.private-tests.receipt.v1");
   cJSON_AddStringToObject(receipt, "config_sha256", field(request, "config_sha256"));
   cJSON_AddStringToObject(receipt, "source_revision", field(request, "source_revision"));
   if (!strstr(mode, "missing-snapshot"))
      cJSON_AddStringToObject(receipt, "snapshot_id",
                              strstr(mode, "wrong-snapshot") ? "ffffffffffffffffffffffffffffffff"
                                                             : field(request, "snapshot_id"));
   if (strcmp(mode, "missing-mount"))
      cJSON_AddBoolToObject(receipt, "mount_identity_verified",
                            strstr(mode, "unverified-mount") == NULL);

   int preflight = !strncmp(mode, "preflight", 9) || !strcmp(mode, "unknown");
   cJSON_AddBoolToObject(receipt, "complete", !preflight);
   if (preflight || !strcmp(mode, "passed-fallback"))
   {
      cJSON_AddBoolToObject(receipt, "ordinary_fallback_eligible", 1);
   }
   if (strcmp(mode, "missing-count"))
      cJSON_AddNumberToObject(
          receipt, "configured_steps_started",
          preflight
              ? (!strcmp(mode, "unknown") ? 1 : 0)
              : (!strcmp(mode, "wrong-count")
                     ? 1
                     : cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(request, "steps"))));
   cJSON_AddBoolToObject(receipt, "quiescent", 1);
   cJSON_AddBoolToObject(receipt, "removal_acknowledged", strcmp(mode, "no-cleanup") != 0);
   cJSON *profile = cJSON_AddObjectToObject(receipt, "profile");
   char id[65];
   memset(id, 'd', 64);
   id[64] = 0;
   cJSON_AddStringToObject(profile, "container_id", id);
   cJSON_AddStringToObject(profile, "profile_sha256", !strcmp(mode, "empty-profile") ? "" : id);
   char image[72];
   snprintf(image, sizeof image, "sha256:%s", id);
   cJSON_AddStringToObject(profile, "image_id", image);
   cJSON *observations = cJSON_AddArrayToObject(receipt, "steps");
   for (int i = 0; i < cJSON_GetArraySize(steps); ++i)
   {
      const cJSON *step = cJSON_GetArrayItem(steps, i);
      cJSON *item = cJSON_CreateObject();
      cJSON_AddItemToArray(observations, item);
      cJSON_AddStringToObject(item, "name", field(step, "name"));
      char digest[65];
      const char *command = field(step, "run");
      assert(!aimee_sha256_hex(command, strlen(command), digest));
      cJSON_AddStringToObject(item, "run_sha256", digest);
      const char *code = !strcmp(mode, "failed") && i == 0 ? "1" : "0";
      cJSON_AddStringToObject(item, "status", !strcmp(code, "0") ? "passed" : "failed");
      cJSON *observation = cJSON_AddObjectToObject(item, "observation");
      cJSON_AddStringToObject(observation, "container_id", id);
      memset(id, 'e' + i, 64);
      if (strcmp(mode, "missing-exec"))
         cJSON_AddStringToObject(observation, "exec_id", id);
      memset(id, 'd', 64);
      cJSON_AddStringToObject(observation, "termination", "exited");
      cJSON_AddBoolToObject(observation, "complete", 1);
      if (strcmp(mode, "bad-last") || i == 0)
         cJSON_AddStringToObject(observation, "exit_code", code);
   }
   char *wire = cJSON_PrintUnformatted(receipt);
   assert(wire);
   fputs(wire, stdout);
   free(wire);
   cJSON_Delete(receipt);
   cJSON_Delete(request);
   return 0;
}
static void reset(const char *mode)
{
   assert(!capture_refs && !mirror_refs && !observer);
   memset(&context, 0, sizeof context);
   snprintf(context.vault_principal, sizeof context.vault_principal, "alice");
   context.vault_transport = ATTEST_UDS_PEERCRED;
   memset(&fixture_config, 0, sizeof fixture_config);
   fixture_config.count = 2;
   snprintf(fixture_config.steps[0].name, MAX_STEP_NAME, "first");
   snprintf(fixture_config.steps[0].run, MAX_STEP_CMD, "%s", mode);
   snprintf(fixture_config.steps[1].name, MAX_STEP_NAME, "second");
   snprintf(fixture_config.steps[1].run, MAX_STEP_CMD, "second-command");
   snapshots = releases = 0;
   source_clean = snapshot_clean = release_ok = observer_ok = accepting = socket_available = 1;
   configured_image = 1;
}
static verify_execution_t start(void)
{
   verify_execution_t execution = {0};
   assert(registered(source_root, "execution-source", &fixture_config, &execution) == 1);
   assert(execution.resource && execution.run && execution.finish);
   assert(capture_refs == 1 && mirror_refs == 1);
   return execution;
}
static void complete(int success, int qualified)
{
   assert(observer && observer_prepare);
   server_session_terminal_action_fn action = observer;
   void *resource = observer_resource;
   const cJSON *proof = success ? observer_prepare(resource) : NULL;
   assert(!!proof == qualified);
   if (proof)
   {
      assert(!strcmp(field(proof, "session_id"), "canonical-session"));
      assert(!strcmp(field(proof, "turn_id"), "canonical-turn"));
      assert(!strcmp(field(proof, "actor"), "alice"));
      assert(!strcmp(field(proof, "scope_id"), "project-one"));
      assert(!strcmp(field(proof, "snapshot_id"), lease_id));
   }
   observer = NULL;
   observer_prepare = NULL;
   observer_resource = NULL;
   action(success, resource);
   assert(!capture_refs && !mirror_refs);
}
static void run_case(const char *mode, int qualified)
{
   reset(mode);
   verify_execution_t execution = start();
   int results[2] = {0, 0};
   char *outputs[2] = {NULL, NULL};
   int handled = execution.run(execution.resource, &fixture_config, results, outputs, NULL);
   assert(handled == (strcmp(mode, "preflight") ? 1 : 0));
   assert(snapshots == 1 && releases == 1);
   for (int i = 0; i < 2; ++i)
   {
      assert(results[i] == (qualified ? (!strcmp(mode, "failed") && i == 0 ? 1 : 0) : -1));
      free(outputs[i]);
   }
   execution.finish(execution.resource);
   if (qualified)
   {
      assert(capture_refs == 1 && mirror_refs == 1);
      complete(1, 1);
   }
   else
      assert(!observer && !capture_refs && !mirror_refs);
}
int main(int argc, char **argv)
{
   if (argc > 1 && !strcmp(argv[1], "verify"))
      return helper(argc, argv);
   char binary[4096];
   assert(realpath(argv[0], binary));
   assert(!setenv("AIMEE_DELEGATE_EGRESS_BIN", binary, 1));
   assert(!unsetenv("AIMEE_SANDBOX_HOST_MOUNTS"));
   char directory[4096];
   snprintf(directory, sizeof directory, "%s/aimee-verification-test-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory));
   snprintf(source_root, sizeof source_root, "%s", directory);
   server_verification_register();
   assert(registered);
   reset("passed");
   verify_execution_t refused = {0};
   context.vault_transport = ATTEST_NONE;
   assert(!registered(source_root, "execution-source", &fixture_config, &refused));
   context.vault_transport = ATTEST_UDS_PEERCRED;
   fixture_config.incomplete = 1;
   assert(!registered(source_root, "execution-source", &fixture_config, &refused));
   fixture_config.incomplete = 0;
   socket_available = 0;
   assert(!registered(source_root, "execution-source", &fixture_config, &refused));
   assert(!capture_refs && !mirror_refs);
   socket_available = 1;
   configured_image = 0;
   assert(!registered(source_root, "execution-source", &fixture_config, &refused));
   assert(!capture_refs && !mirror_refs);
   fixture_config.env_count = 1;
   snprintf(fixture_config.env_checks[0], MAX_STEP_NAME, "bash");
   verify_execution_t defaulted = start();
   assert(!strcmp(((verification_t *)defaulted.resource)->image, "ubuntu:22.04"));
   defaulted.finish(defaulted.resource);
   run_case("passed", 1);
   run_case("missing-count", 0);
   run_case("wrong-count", 0);
   run_case("failed", 1);
   run_case("malformed", 0);
   run_case("no-cleanup", 0);
   run_case("bad-last", 0);
   run_case("empty-profile", 0);
   run_case("missing-exec", 0);
   run_case("preflight", 0);
   run_case("unknown", 0);
   run_case("passed-fallback", 0);
   run_case("wrong-snapshot", 0);
   run_case("missing-snapshot", 0);
   run_case("unverified-mount", 0);
   run_case("missing-mount", 0);
   run_case("preflight-wrong-snapshot", 0);
   run_case("preflight-unverified-mount", 0);

   reset("passed");
   fixture_config.env_count = 1;
   snprintf(fixture_config.env_checks[0], MAX_STEP_NAME, "bash");
   verify_execution_t unchecked_environment = start();
   int unchecked_results[2] = {0, 0};
   char *unchecked_outputs[2] = {NULL, NULL};
   unchecked_environment.run(unchecked_environment.resource, &fixture_config, unchecked_results,
                             unchecked_outputs, NULL);
   assert(unchecked_results[0] == -1 && unchecked_results[1] == -1);
   free(unchecked_outputs[0]);
   free(unchecked_outputs[1]);
   unchecked_environment.finish(unchecked_environment.resource);
   assert(!observer && !capture_refs && !mirror_refs);

   for (int scenario = 0; scenario < 4; ++scenario)
   {
      reset("passed");
      verify_execution_t execution = start();
      int results[2] = {-1, -1};
      char *outputs[2] = {NULL, NULL};
      execution.run(execution.resource, &fixture_config, results, outputs, NULL);
      free(outputs[0]);
      free(outputs[1]);
      if (scenario == 3)
         observer_ok = 0;
      execution.finish(execution.resource);
      if (scenario == 3)
      {
         assert(!observer && !capture_refs && !mirror_refs);
         continue;
      }
      if (scenario == 0)
         source_clean = 0;
      else if (scenario == 1)
         fixture_config.steps[1].run[0] = 'X';
      complete(scenario != 2, 0);
   }
   reset("slow");
   verify_execution_t execution = start();
   volatile int cancelled = 1;
   int results[2] = {0, 0};
   char *outputs[2] = {NULL, NULL};
   assert(execution.run(execution.resource, &fixture_config, results, outputs, &cancelled) == 1);
   assert(results[0] == -1 && results[1] == -1);
   free(outputs[0]);
   free(outputs[1]);
   execution.finish(execution.resource);
   assert(!observer && !capture_refs && !mirror_refs);
   assert(!rmdir(directory));
   puts("server verification controller: passed");
   return 0;
}
