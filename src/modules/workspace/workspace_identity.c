/* Canonical repository identity: shared by thin-client and server owners.
 * No registry/config/store access. Filesystem/manifest/Git facts retain their
 * existing precedence and private persisted UUID custody. */
#include <aimee/workspace/workspace.h>
#include "workspace_identity.h"
#include "workspace_manifest.h"
#include "report_enrichment.h"
#include "util_url.h"
#include "platform_random.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static int workspace_identity_value_ok(const char *id)
{
   if (!id || !id[0] || strlen(id) >= 128)
      return 0;
   for (const unsigned char *p = (const unsigned char *)id; *p; p++)
      if (!(isalnum(*p) || *p == '.' || *p == '_' || *p == ':' || *p == '/' || *p == '@' ||
            *p == '+' || *p == '-'))
         return 0;
   return 1;
}

static int workspace_copy_identity(char *out, size_t out_len, const char *id)
{
   if (!out)
      return 0;
   if (out_len == 0 || !id || strlen(id) >= out_len)
      return -1;
   snprintf(out, out_len, "%s", id);
   return 0;
}

int workspace_identity_git_line(const char *root, const char *arg, char *out, size_t out_len)
{
   if (!root || !root[0] || !arg || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   const char *argv[] = {"git", "-C", root, "rev-parse", arg, NULL};
   char *captured = NULL;
   if (safe_exec_capture(argv, &captured, MAX_PATH_LEN * 2) != 0 || !captured)
   {
      free(captured);
      return -1;
   }
   size_t n = strcspn(captured, "\r\n");
   if (n >= out_len)
      n = out_len - 1;
   memcpy(out, captured, n);
   out[n] = '\0';
   free(captured);
   return out[0] ? 0 : -1;
}

static void workspace_project_root(const char *cwd, char *out, size_t out_len)
{
   if (workspace_identity_git_line(cwd, "--show-toplevel", out, out_len) == 0)
      return;
   char resolved[MAX_PATH_LEN];
   snprintf(out, out_len, "%s", realpath(cwd, resolved) ? resolved : cwd);
}

static void workspace_manifest_repo_path(const char *manifest_dir, const manifest_repo_t *repo,
                                         char *out, size_t out_len)
{
   if (repo->path[0])
   {
      if (repo->path[0] == '/')
         snprintf(out, out_len, "%s", repo->path);
      else
         snprintf(out, out_len, "%s/%s", manifest_dir, repo->path);
      return;
   }
   const char *base = strrchr(repo->url, '/');
   base = base ? base + 1 : repo->url;
   char name[256];
   snprintf(name, sizeof(name), "%s", base);
   size_t len = strlen(name);
   if (len > 4 && strcmp(name + len - 4, ".git") == 0)
      name[len - 4] = '\0';
   snprintf(out, out_len, "%s/%s", manifest_dir, name);
}

/* Search the project and its workspace ancestors. A per-repository id wins;
 * a top-level id applies only when the manifest directory is the project root,
 * which prevents one workspace id from collapsing a multi-repo workspace. */
static int workspace_manifest_identity(const char *project_root, char *out, size_t out_len)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", project_root);
   for (int depth = 0; depth <= MAX_WORKSPACE_DEPTH && dir[0]; depth++)
   {
      workspace_manifest_t manifest;
      int load_rc = workspace_manifest_load(dir, &manifest);
      if (load_rc == -2)
         return -2;
      if (load_rc == 0)
      {
         for (int i = 0; i < manifest.repo_count; i++)
         {
            if (!workspace_identity_value_ok(manifest.repos[i].id))
               continue;
            char declared[MAX_PATH_LEN], resolved[MAX_PATH_LEN];
            workspace_manifest_repo_path(dir, &manifest.repos[i], declared, sizeof(declared));
            const char *candidate = realpath(declared, resolved) ? resolved : declared;
            if (strcmp(candidate, project_root) == 0)
            {
               snprintf(out, out_len, "%s", manifest.repos[i].id);
               return 0;
            }
         }
         if (strcmp(dir, project_root) == 0 && workspace_identity_value_ok(manifest.id))
         {
            snprintf(out, out_len, "%s", manifest.id);
            return 0;
         }
      }
      char *slash = strrchr(dir, '/');
      if (!slash || slash == dir)
         break;
      *slash = '\0';
   }
   return -1;
}

static int workspace_read_project_id(const char *path, char *out, size_t out_len)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) != 0)
   {
      close(fd);
      return -1;
   }
   char id[64];
   ssize_t n = read(fd, id, sizeof(id) - 1);
   close(fd);
   if (n <= 0)
      return -1;
   id[n] = '\0';
   id[strcspn(id, "\r\n")] = '\0';
   if (strlen(id) != 36)
      return -1;
   for (size_t i = 0; id[i]; i++)
   {
      int hyphen = i == 8 || i == 13 || i == 18 || i == 23;
      if ((hyphen && id[i] != '-') ||
          (!hyphen && !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))))
         return -1;
   }
   snprintf(out, out_len, "local:%s", id);
   return 0;
}

static int workspace_fsync_dir(const char *path)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
   if (fd < 0)
      return -1;
   int rc = fsync(fd);
   close(fd);
   return rc;
}

static int workspace_persisted_identity(const char *project_root, char *out, size_t out_len)
{
   char common[MAX_PATH_LEN] = "";
   char dir[MAX_PATH_LEN];
   char path[MAX_PATH_LEN];
   if (workspace_identity_git_line(project_root, "--git-common-dir", common, sizeof(common)) == 0)
   {
      if (common[0] == '/')
         snprintf(dir, sizeof(dir), "%s", common);
      else
         snprintf(dir, sizeof(dir), "%s/%s", project_root, common);
      snprintf(path, sizeof(path), "%s/aimee-project-id", dir);
   }
   else
   {
      snprintf(dir, sizeof(dir), "%s/.aimee", project_root);
      struct stat st;
      if (lstat(dir, &st) != 0)
      {
         if (mkdir(dir, 0700) != 0 && errno != EEXIST)
            return -1;
         if (workspace_fsync_dir(project_root) != 0)
            return -1;
      }
      else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
         return -1;
      snprintf(path, sizeof(path), "%s/project-id", dir);
   }

   if (workspace_read_project_id(path, out, out_len) == 0)
      return 0;

   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;
   raw[6] = (unsigned char)((raw[6] & 0x0f) | 0x40); /* RFC 4122 v4 */
   raw[8] = (unsigned char)((raw[8] & 0x3f) | 0x80);
   char id[37];
   snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd >= 0)
   {
      size_t len = strlen(id);
      int ok = write(fd, id, len) == (ssize_t)len && write(fd, "\n", 1) == 1 && fsync(fd) == 0;
      close(fd);
      if (!ok)
      {
         unlink(path);
         workspace_fsync_dir(dir);
         return -1;
      }
      if (workspace_fsync_dir(dir) != 0)
         return -1;
   }
   else if (errno != EEXIST)
      return -1;

   return workspace_read_project_id(path, out, out_len);
}

int workspace_repo_identity(const char *cwd, char *project_out, size_t project_len,
                            char *workspace_out, size_t workspace_len)
{
   if (project_out && project_len > 0)
      project_out[0] = '\0';
   if (workspace_out && workspace_len > 0)
      workspace_out[0] = '\0';
   if (!cwd || !cwd[0])
      return -1;

   char project_root[MAX_PATH_LEN];
   workspace_project_root(cwd, project_root, sizeof(project_root));

   char identity[512] = "";
   int manifest_rc = workspace_manifest_identity(project_root, identity, sizeof(identity));
   if (manifest_rc == 0)
   {
      if (workspace_copy_identity(project_out, project_len, identity) != 0 ||
          workspace_copy_identity(workspace_out, workspace_len, identity) != 0)
         goto fail;
      return 0;
   }
   if (manifest_rc == -2)
      goto fail;

   /* Canonical forge identity: reads the configured remote and normalizes any
    * transport/case to https://host/owner/repo. Path-derived local identities
    * are deliberately replaced by a persisted UUID below. */
   report_subject_t subj;
   int remote_identity = report_subject_from_project_root(project_root, &subj) == 0 && subj.id[0] &&
                         strncmp(subj.id, "local:", 6) != 0;
   if (!remote_identity)
   {
      if (workspace_persisted_identity(project_root, identity, sizeof(identity)) != 0)
         goto fail;
      if (workspace_copy_identity(project_out, project_len, identity) != 0 ||
          workspace_copy_identity(workspace_out, workspace_len, identity) != 0)
         goto fail;
      return 0;
   }

   /* Project scope = the repository itself, not its checkout path, so clones on
    * different machines share one scope. */
   if (workspace_copy_identity(project_out, project_len, subj.id) != 0)
      goto fail;

   /* Workspace scope = the org/namespace parent (https://host/owner) when the
    * identity has one; local-only repos have no parent, so scope the workspace
    * to the repo identity itself rather than reintroducing a machine path. */
   if (workspace_out && workspace_len > 0)
   {
      char *parent = util_url_workspace_parent(subj.id);
      int copy_rc =
          workspace_copy_identity(workspace_out, workspace_len, parent ? parent : subj.id);
      free(parent);
      if (copy_rc != 0)
         goto fail;
   }
   return 0;

fail:
   if (project_out && project_len > 0)
      project_out[0] = '\0';
   if (workspace_out && workspace_len > 0)
      workspace_out[0] = '\0';
   return -1;
}

int workspace_repo_index_keys(const char *root, const char *fallback_workspace, char *name_out,
                              size_t name_len, char *ws_out, size_t ws_len)
{
   (void)fallback_workspace;
   if (workspace_repo_identity(root, name_out, name_len, ws_out, ws_len) == 0 && name_out &&
       name_out[0])
      return 0;
   if (name_out && name_len > 0)
      name_out[0] = '\0';
   if (ws_out && ws_len > 0)
      ws_out[0] = '\0';
   return -1;
}
