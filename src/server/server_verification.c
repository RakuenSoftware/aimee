/* Canonical turn owner for private configured-test execution. Test text and
 * child exit status alone never grant a task grade. */
#include "server_verification.h"
#include "modules/git/git_verify.h"
#include "request_context.h"
#include "server_session_capture.h"
#include "module_json_call.h"
#include "aimee_sha256.h"
#include "aimee_home.h"
#include "util.h"
#include <aimee/git/module_api.h>
#include <aimee/delegates/delegate_sandbox_image.h>
#include <aimee/delegates/delegate_backend_docker.h>
#include "modules/workspace/workspace_turn.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERIFICATION_RECEIPT_MAX (2u * 1024u * 1024u)
typedef struct
{
   server_session_capture_t *capture;
   workspace_turn_mirror_lease_t *mirror;
   int source_fd;
   char root[4096], actor[129], project[257], session[129], turn[129];
   char commit[65], tree[65], config[65], device[32], inode[32], lease[33];
   char snapshot[4096], image[256];
   cJSON *proof;
} verification_t;

static const char *field(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   return value ? value : "";
}
static int hex_value(const char *value, size_t length)
{
   if (strlen(value) != length)
      return 0;
   for (size_t i = 0; i < length; ++i)
      if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
         return 0;
   return 1;
}
static cJSON *snapshot_call(verification_t *v, const char *op)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "op", op);
   if (!strcmp(op, "snapshot-create") || !strcmp(op, "snapshot-source-state"))
   {
      cJSON_AddStringToObject(request, "project_root", v->root);
      cJSON_AddStringToObject(request, "device", v->device);
      cJSON_AddStringToObject(request, "inode", v->inode);
      cJSON_AddStringToObject(request, "commit", v->commit);
      cJSON_AddStringToObject(request, "tree", v->tree);
   }
   else
      cJSON_AddStringToObject(request, "lease", v->lease);
   cJSON *reply =
       aimee_module_json_call(AIMEE_GIT_EVENT_VERIFY_RUN, AIMEE_GIT_STAGE_VERIFY_RUN, request,
                              16384, !strcmp(op, "snapshot-renew") ? 1000 : 60000, NULL);
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")))
   {
      cJSON_Delete(reply);
      return NULL;
   }
   return reply;
}
static cJSON *configuration(const verify_config_t *cfg)
{
   cJSON *result = cJSON_CreateObject(), *steps = cJSON_AddArrayToObject(result, "steps");
   if (!result || !steps)
   {
      cJSON_Delete(result);
      return NULL;
   }
   cJSON_AddNumberToObject(result, "incremental", cfg->incremental);
   cJSON_AddNumberToObject(result, "enforce", cfg->enforce);
   cJSON_AddStringToObject(result, "always_run_globs", cfg->always_run_globs);
   cJSON *environment = cJSON_AddArrayToObject(result, "env_checks");
   for (int i = 0; i < cfg->env_count; ++i)
      cJSON_AddItemToArray(environment, cJSON_CreateString(cfg->env_checks[i]));
   cJSON_AddNumberToObject(result, "incomplete", cfg->incomplete);
   for (int i = 0; i < cfg->count; ++i)
   {
      const verify_step_t *step = &cfg->steps[i];
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", step->name);
      cJSON_AddStringToObject(item, "run", step->run);
      cJSON_AddStringToObject(item, "after", step->after);
      cJSON_AddStringToObject(item, "paths", step->paths);
      cJSON_AddNumberToObject(item, "scope_changed", step->scope_changed);
      cJSON_AddStringToObject(item, "tier", step->tier);
      cJSON_AddItemToArray(steps, item);
   }
   return result;
}
static int configuration_digest(const verify_config_t *cfg, char digest[65])
{
   cJSON *value = configuration(cfg);
   char *wire = value ? cJSON_PrintUnformatted(value) : NULL;
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), digest) : -1;
   free(wire);
   cJSON_Delete(value);
   return rc;
}
static int source_current(verification_t *v)
{
   verify_config_t cfg;
   char digest[65];
   cJSON *state = snapshot_call(v, "snapshot-source-state");
   int valid = state && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state, "clean")) &&
               !strcmp(field(state, "commit"), v->commit) &&
               !strcmp(field(state, "tree"), v->tree) && !verify_load_config(v->root, &cfg) &&
               !configuration_digest(&cfg, digest) && !strcmp(digest, v->config);
   cJSON_Delete(state);
   return valid;
}
static void verification_free(verification_t *v)
{
   if (!v)
      return;
   if (v->lease[0])
      cJSON_Delete(snapshot_call(v, "snapshot-release"));
   if (v->source_fd >= 0)
      close(v->source_fd);
   workspace_turn_mirror_release(v->mirror);
   server_session_capture_release(v->capture);
   cJSON_Delete(v->proof);
   free(v);
}
static const cJSON *prepare_terminal(void *resource)
{
   verification_t *v = resource;
   return v->proof && source_current(v) ? v->proof : NULL;
}
static void terminal(int success, void *resource)
{
   (void)success;
   verification_free(resource);
}
static int mount_table(char out[8192])
{
   out[0] = 0;
   const char *mapping = getenv("AIMEE_SANDBOX_HOST_MOUNTS");
   size_t at = 0;
   for (const char *p = mapping; p && *p;)
   {
      const char *comma = strchr(p, ',');
      size_t length = comma ? (size_t)(comma - p) : strlen(p);
      const char *eq = memchr(p, '=', length);
      if (!eq || eq == p || eq == p + length - 1 || memchr(p, '\n', length) ||
          memchr(p, '\t', length))
         return -1;
      int n = snprintf(out + at, 8192 - at, "%.*s\t%.*s\n", (int)(eq - p), p,
                       (int)(p + length - eq - 1), eq + 1);
      if (n < 0 || (size_t)n >= 8192 - at)
         return -1;
      at += (size_t)n;
      p = comma ? comma + 1 : NULL;
   }
   return 0;
}
static double monotonic_seconds(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
/* File-backed private stdin/stdout avoids deadlocking on a full pipe. The
 * helper owns remote cancellation/cleanup; give SIGTERM its bounded join grace. */
static cJSON *execute(verification_t *v, const verify_config_t *cfg, volatile int *cancel)
{
   cJSON *request = cJSON_CreateObject(), *steps = cJSON_AddArrayToObject(request, "steps");
   cJSON_AddStringToObject(request, "schema", "aimee.private-tests.request.v1");
   cJSON_AddStringToObject(request, "config_sha256", v->config);
   cJSON_AddStringToObject(request, "source_revision", v->commit);
   cJSON_AddStringToObject(request, "snapshot_id", v->lease);
   cJSON_AddNumberToObject(request, "step_timeout_ms", 1800000);
   cJSON *environment = cJSON_AddArrayToObject(request, "env_checks");
   for (int i = 0; i < cfg->env_count; ++i)
      cJSON_AddItemToArray(environment, cJSON_CreateString(cfg->env_checks[i]));
   for (int i = 0; i < cfg->count; ++i)
   {
      cJSON *step = cJSON_CreateObject();
      cJSON_AddStringToObject(step, "name", cfg->steps[i].name);
      cJSON_AddStringToObject(step, "run", cfg->steps[i].run);
      cJSON_AddStringToObject(step, "after", cfg->steps[i].after);
      cJSON_AddItemToArray(steps, step);
   }
   char *wire = cJSON_PrintUnformatted(request);
   cJSON_Delete(request);
   FILE *input = tmpfile(), *output = tmpfile();
   cJSON *receipt = NULL;
   char table[8192], user[64], parent[32], owner_state[4096];
   const char *home = aimee_home();
   int owner_length =
       home ? snprintf(owner_state, sizeof owner_state, "%s/verification-workers", home) : -1;
   if (!home || home[0] != '/' || owner_length < 0 || (size_t)owner_length >= sizeof owner_state)
      goto done;
   if (!wire || !input || !output || mount_table(table) ||
       fwrite(wire, 1, strlen(wire), input) != strlen(wire) || fflush(input) ||
       fseek(input, 0, SEEK_SET))
      goto done;
   if (fcntl(fileno(input), F_SETFD, FD_CLOEXEC) || fcntl(fileno(output), F_SETFD, FD_CLOEXEC))
      goto done;
   snprintf(user, sizeof user, "%lu:%lu", (unsigned long)geteuid(), (unsigned long)getegid());
   snprintf(parent, sizeof parent, "%ld", (long)getpid());
   const char *helper = getenv("AIMEE_DELEGATE_EGRESS_BIN");
   if (!helper || !helper[0])
      helper = "aimee-delegate-egress";
   char *argv[] = {(char *)helper,
                   "verify",
                   "--owner-state-dir",
                   owner_state,
                   "--parent-pid",
                   parent,
                   "--engine-socket",
                   "/var/run/docker.sock",
                   "--image",
                   v->image,
                   "--workdir",
                   v->snapshot,
                   "--user",
                   user,
                   "--mount-table",
                   table,
                   NULL};
   char *env[] = {"PATH=/usr/local/bin:/usr/bin:/bin", NULL};
   posix_spawn_file_actions_t actions;
   if (posix_spawn_file_actions_init(&actions))
      goto done;
   int err = posix_spawn_file_actions_adddup2(&actions, fileno(input), STDIN_FILENO);
   if (!err)
      err = posix_spawn_file_actions_adddup2(&actions, fileno(output), STDOUT_FILENO);
   if (!err)
      err = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
   pid_t pid;
   if (!err)
      err = posix_spawnp(&pid, helper, &actions, NULL, argv, env);
   posix_spawn_file_actions_destroy(&actions);
   if (err)
      goto done;
   int status = 0, valid = 1;
   double start = monotonic_seconds(), renewed = start, stopped = 0;
   for (;;)
   {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid)
         break;
      if (waited < 0 && errno != EINTR)
      {
         valid = 0;
         break;
      }
      double now = monotonic_seconds();
      if (now - renewed >= 30 && !stopped)
      {
         cJSON *reply = snapshot_call(v, "snapshot-renew");
         if (!reply)
            valid = 0;
         cJSON_Delete(reply);
         renewed = now;
      }
      if (!stopped &&
          (!valid || (cancel && *cancel) || !server_session_capture_accepting(v->capture) ||
           now - start > cfg->count * 1800.0 + 60))
      {
         kill(pid, SIGTERM);
         stopped = now;
         valid = 0;
      }
      if (stopped && now - stopped > 17)
      {
         kill(pid, SIGKILL);
         while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
         {
         }
         break;
      }
      struct timespec pause = {.tv_nsec = 100000000};
      nanosleep(&pause, NULL);
   }
   if (!valid || !WIFEXITED(status) || WEXITSTATUS(status))
      goto done;
   struct stat stat;
   if (fstat(fileno(output), &stat) || stat.st_size < 1 ||
       stat.st_size > VERIFICATION_RECEIPT_MAX || fseek(output, 0, SEEK_SET))
      goto done;
   char *body = calloc((size_t)stat.st_size + 1, 1);
   if (body && fread(body, 1, (size_t)stat.st_size, output) == (size_t)stat.st_size)
      receipt = cJSON_ParseWithOpts(body, NULL, 1);
   free(body);
done:
   free(wire);
   if (input)
      fclose(input);
   if (output)
      fclose(output);
   return receipt;
}
/* Byte tails remain binary in the owner receipt. This explicit human log
 * boundary replaces NUL/control bytes; it is never test-result authority. */
static char *display_output(const cJSON *observation)
{
   char *text = calloc(32771, 1);
   if (!text)
      return NULL;
   size_t at = 0;
   const char *keys[] = {"stdout_base64", "stderr_base64"};
   for (size_t n = 0; n < 2; ++n)
   {
      unsigned char bytes[16384];
      size_t count = aimee_base64_decode(field(observation, keys[n]), bytes, sizeof bytes);
      if (count == (size_t)-1)
         continue;
      for (size_t i = 0; i < count; ++i)
         text[at++] =
             (bytes[i] >= 32 || bytes[i] == '\n' || bytes[i] == '\t') ? (char)bytes[i] : '?';
      text[at++] = '\n';
   }
   text_sanitize_utf8(text);
   return text;
}
static int run(void *resource, const verify_config_t *cfg, int *results, char **outputs,
               volatile int *cancel)
{
   verification_t *v = resource;
   int declined = 0;
   cJSON *state = snapshot_call(v, "snapshot-create"), *receipt = NULL;
   if (!state || strlen(field(state, "lease")) != 32 ||
       strlen(field(state, "path")) >= sizeof v->snapshot)
      goto done;
   snprintf(v->lease, sizeof v->lease, "%s", field(state, "lease"));
   snprintf(v->snapshot, sizeof v->snapshot, "%s", field(state, "path"));
   cJSON_Delete(state);
   state = NULL;
   if (!source_current(v))
      goto done;
   receipt = execute(v, cfg, cancel);
   const cJSON *started = cJSON_GetObjectItemCaseSensitive(receipt, "configured_steps_started");
   declined =
       receipt && !strcmp(field(receipt, "schema"), "aimee.private-tests.receipt.v1") &&
       !strcmp(field(receipt, "config_sha256"), v->config) &&
       !strcmp(field(receipt, "source_revision"), v->commit) &&
       !strcmp(field(receipt, "snapshot_id"), v->lease) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "mount_identity_verified")) &&
       cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(receipt, "complete")) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "ordinary_fallback_eligible")) &&
       cJSON_IsNumber(started) && started->valuedouble == 0 &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "quiescent")) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "removal_acknowledged")) &&
       !(cancel && *cancel) && server_session_capture_accepting(v->capture) && source_current(v);
   const cJSON *steps = cJSON_GetObjectItemCaseSensitive(receipt, "steps");
   const cJSON *profile = cJSON_GetObjectItemCaseSensitive(receipt, "profile");
   int valid =
       receipt && !strcmp(field(receipt, "schema"), "aimee.private-tests.receipt.v1") &&
       !strcmp(field(receipt, "config_sha256"), v->config) &&
       !strcmp(field(receipt, "source_revision"), v->commit) &&
       !strcmp(field(receipt, "snapshot_id"), v->lease) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "mount_identity_verified")) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "complete")) &&
       !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "ordinary_fallback_eligible")) &&
       (cJSON_IsNumber(started) && started->valuedouble == cfg->count) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "quiescent")) &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(receipt, "removal_acknowledged")) &&
       cJSON_IsArray(steps) && cJSON_GetArraySize(steps) == cfg->count &&
       hex_value(field(profile, "container_id"), 64) &&
       hex_value(field(profile, "profile_sha256"), 64) &&
       !strncmp(field(profile, "image_id"), "sha256:", 7) &&
       hex_value(field(profile, "image_id") + 7, 64);
   const cJSON *checks = cJSON_GetObjectItemCaseSensitive(receipt, "env_checks");
   valid = valid && cJSON_GetArraySize(checks) == cfg->env_count;
   for (int i = 0; i < cfg->env_count && valid; ++i)
   {
      const cJSON *check = cJSON_GetArrayItem(checks, i);
      const cJSON *observation = cJSON_GetObjectItemCaseSensitive(check, "observation");
      char command[MAX_STEP_NAME + 32], digest[65];
      snprintf(command, sizeof command, "command -v -- %s", cfg->env_checks[i]);
      valid = !aimee_sha256_hex(command, strlen(command), digest) &&
              !strcmp(field(check, "name"), cfg->env_checks[i]) &&
              !strcmp(field(check, "run_sha256"), digest) &&
              !strcmp(field(check, "status"), "passed") &&
              !strcmp(field(observation, "container_id"), field(profile, "container_id")) &&
              hex_value(field(observation, "exec_id"), 64) &&
              !strcmp(field(observation, "termination"), "exited") &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(observation, "complete")) &&
              !strcmp(field(observation, "exit_code"), "0");
   }
   state = snapshot_call(v, "snapshot-state");
   valid = valid && state && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state, "clean")) &&
           !strcmp(field(state, "commit"), v->commit) && !strcmp(field(state, "tree"), v->tree);
   cJSON *proof = cJSON_CreateObject(), *proof_steps = cJSON_AddArrayToObject(proof, "steps");
   for (int i = 0; i < cfg->count && valid; ++i)
   {
      const cJSON *step = cJSON_GetArrayItem(steps, i);
      const cJSON *observation = cJSON_GetObjectItemCaseSensitive(step, "observation");
      char digest[65], *end;
      const char *exit = field(observation, "exit_code");
      errno = 0;
      long code = strtol(exit, &end, 10);
      char canonical[16];
      snprintf(canonical, sizeof canonical, "%ld", code);
      valid = !aimee_sha256_hex(cfg->steps[i].run, strlen(cfg->steps[i].run), digest) &&
              !strcmp(field(step, "name"), cfg->steps[i].name) &&
              !strcmp(field(step, "run_sha256"), digest) &&
              !strcmp(field(observation, "container_id"), field(profile, "container_id")) &&
              hex_value(field(observation, "exec_id"), 64) &&
              !strcmp(field(observation, "termination"), "exited") &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(observation, "complete")) && exit[0] &&
              !errno && !*end && code >= 0 && code < 124 && !strcmp(exit, canonical) &&
              !strcmp(field(step, "status"), code ? "failed" : "passed");
      if (!valid)
         break;
      results[i] = (int)code;
      outputs[i] = display_output(observation);
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", cfg->steps[i].name);
      cJSON_AddStringToObject(item, "command_sha256", digest);
      cJSON_AddBoolToObject(item, "ran", 1);
      cJSON_AddBoolToObject(item, "skipped", 0);
      cJSON_AddStringToObject(item, "termination", "exited");
      cJSON_AddStringToObject(item, "exit_code", exit);
      cJSON_AddItemToArray(proof_steps, item);
   }
   if (valid && source_current(v))
   {
      cJSON_AddNumberToObject(proof, "version", 1);
      cJSON_AddStringToObject(proof, "session_id", v->session);
      cJSON_AddStringToObject(proof, "turn_id", v->turn);
      cJSON_AddStringToObject(proof, "actor", v->actor);
      cJSON_AddStringToObject(proof, "scope_kind", "project");
      cJSON_AddStringToObject(proof, "scope_id", v->project);
      cJSON_AddStringToObject(proof, "isolation", "private_source_snapshot");
      cJSON_AddStringToObject(proof, "snapshot_id", v->lease);
      cJSON_AddStringToObject(proof, "snapshot_commit", v->commit);
      cJSON_AddStringToObject(proof, "final_commit", v->commit);
      cJSON_AddStringToObject(proof, "snapshot_tree", v->tree);
      cJSON_AddStringToObject(proof, "final_tree", v->tree);
      cJSON_AddStringToObject(proof, "configuration_sha256", v->config);
      cJSON_AddStringToObject(proof, "final_configuration_sha256", v->config);
      cJSON_AddStringToObject(proof, "profile_sha256", field(profile, "profile_sha256"));
      cJSON_AddBoolToObject(proof, "completed", 1);
      time_t now = time(NULL);
      struct tm tm;
      char timestamp[32];
      gmtime_r(&now, &tm);
      strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", &tm);
      cJSON_AddStringToObject(proof, "observed_at", timestamp);
      v->proof = proof;
   }
   else
      cJSON_Delete(proof);
done:
   cJSON_Delete(state);
   cJSON_Delete(receipt);
   /* The worker has been removed. The snapshot is no longer needed after its
    * final measurement; immutable source IDs remain in the proof. */
   if (v->lease[0])
   {
      cJSON *released = snapshot_call(v, "snapshot-release");
      if (!released)
      {
         declined = 0;
         cJSON_Delete(v->proof);
         v->proof = NULL;
      }
      cJSON_Delete(released);
      v->lease[0] = 0;
   }
   if (!v->proof)
      for (int i = 0; i < cfg->count; ++i)
      {
         results[i] = -1;
         free(outputs[i]);
         outputs[i] =
             strdup("private verification unavailable: no complete source-bound receipt\n");
      }
   return declined && !v->proof ? 0 : 1;
}
static void finish(void *resource)
{
   verification_t *v = resource;
   if (v->proof &&
       !server_session_capture_observe_verification(v->capture, prepare_terminal, terminal, v))
      return;
   verification_free(v);
}
static int begin(const char *root, const char *session, const verify_config_t *cfg,
                 verify_execution_t *out)
{
   const request_context_t *owner = request_context_get();
   struct stat socket;
   if (!root || !session || !owner || !owner->vault_principal[0] ||
       owner->vault_transport == ATTEST_NONE || !cfg || cfg->incomplete || cfg->incremental ||
       cfg->count < 1 || cfg->count > MAX_VERIFY_STEPS || stat("/var/run/docker.sock", &socket) ||
       !S_ISSOCK(socket.st_mode))
      return 0;
   for (int i = 0; i < cfg->count; ++i)
      if (cfg->steps[i].scope_changed)
         return 0;
   verification_t *v = calloc(1, sizeof *v);
   if (!v)
      return 0;
   v->source_fd = -1;
   char identity[65];
   if (strlen(root) >= sizeof v->root || strlen(session) >= sizeof v->session ||
       server_session_capture_retain_current(session, owner->vault_principal, &v->capture) ||
       !v->capture || workspace_turn_resource_identity(root, &v->source_fd, identity) ||
       v->source_fd < 0 || workspace_turn_mirror_capture(root, &v->mirror) ||
       server_learning_project_from_cwd(root, v->project, sizeof v->project) || !v->project[0] ||
       configuration_digest(cfg, v->config))
      goto unavailable;
   if (delegate_sandbox_resolve_image(root, v->image, sizeof v->image) || !v->image[0])
   {
      if (!cfg->env_count)
         goto unavailable;
      snprintf(v->image, sizeof v->image, "%s", delegate_backend_docker_default_image());
   }
   const char *destination = server_session_capture_session(v->capture);
   const char *turn = server_session_capture_turn(v->capture);
   if (!destination || !turn || strlen(destination) >= sizeof v->session ||
       strlen(turn) >= sizeof v->turn)
      goto unavailable;
   snprintf(v->root, sizeof v->root, "%s", root);
   snprintf(v->actor, sizeof v->actor, "%s", owner->vault_principal);
   snprintf(v->session, sizeof v->session, "%s", destination);
   snprintf(v->turn, sizeof v->turn, "%s", turn);
   struct stat source;
   if (fstat(v->source_fd, &source))
      goto unavailable;
   snprintf(v->device, sizeof v->device, "%llu", (unsigned long long)source.st_dev);
   snprintf(v->inode, sizeof v->inode, "%llu", (unsigned long long)source.st_ino);
   /* Ordinary metadata selects the candidate; sanitized owner measurement below
    * must independently confirm it against this held source identity. */
   char held[128];
   snprintf(held, sizeof held, "/proc/%ld/fd/%d", (long)getpid(), v->source_fd);
   extern char *verify_compute_commit_hash(const char *project_root);
   char *commit = verify_compute_commit_hash(held), *tree = verify_compute_file_hash(held);
   int valid = commit && tree && strlen(commit) < sizeof v->commit && strlen(tree) < sizeof v->tree;
   if (valid)
   {
      snprintf(v->commit, sizeof v->commit, "%s", commit);
      snprintf(v->tree, sizeof v->tree, "%s", tree);
   }
   free(commit);
   free(tree);
   if (!valid || !source_current(v))
      goto unavailable;
   *out = (verify_execution_t){.resource = v, .run = run, .finish = finish};
   return 1;
unavailable:
   verification_free(v);
   return 0;
}
void server_verification_register(void)
{
   verify_register_execution_owner(begin);
}
