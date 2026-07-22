#include "delegate_source_authority.h"
#include "commands.h"
#include "log.h"
#include "platform_process.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void delegate_check_file_drift(const char *wt_path, const char **named_files, int named_count)
{
   if (!wt_path || !wt_path[0] || named_count <= 0)
      return;

   char cmd[MAX_PATH_LEN + 64];
   snprintf(cmd, sizeof(cmd), "git -C '%s' diff --name-only HEAD 2>/dev/null", wt_path);
   int cmd_rc;
   char *diff_out = run_cmd(cmd, &cmd_rc);

   for (int i = 0; i < named_count; i++)
   {
      if (!named_files[i] || !named_files[i][0])
         continue;
      struct stat st;
      if (stat(named_files[i], &st) != 0)
         continue;
      const char *bn = strrchr(named_files[i], '/');
      bn = bn ? bn + 1 : named_files[i];
      int found = diff_out && (strstr(diff_out, named_files[i]) || strstr(diff_out, bn));
      if (!found)
      {
         LOG_WARN("delegate", "named-file drift: '%s' was explicitly named but not modified",
                  named_files[i]);
         fprintf(stderr, "aimee: warn: delegate did not modify explicitly named file '%s'\n",
                 named_files[i]);
      }
   }
   free(diff_out);
}

void delegate_source_path_add(char paths[][MAX_PATH_LEN], int *count, int max, const char *path)
{
   if (!paths || !count || *count >= max || !path || !path[0])
      return;
   for (int i = 0; i < *count; i++)
   {
      if (strcmp(paths[i], path) == 0)
         return;
   }
   snprintf(paths[*count], MAX_PATH_LEN, "%s", path);
   (*count)++;
}

void delegate_source_authority_env_set(const char paths[][MAX_PATH_LEN], int count,
                                       const char *worktree_root)
{
   if (worktree_root && worktree_root[0])
      platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", worktree_root);
   if (count <= 0)
   {
      platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "");
      platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", "");
      return;
   }

   char buf[32768];
   size_t pos = 0;
   for (int i = 0; i < count && pos < sizeof(buf) - 2; i++)
   {
      int n = snprintf(buf + pos, sizeof(buf) - pos, "%s%s", i ? "\n" : "", paths[i]);
      if (n <= 0)
         break;
      if ((size_t)n >= sizeof(buf) - pos)
      {
         pos = sizeof(buf) - 1;
         break;
      }
      pos += (size_t)n;
   }
   buf[pos] = '\0';
   platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "1");
   platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", buf);
}

char *delegate_append_source_authority_note(const char *base, int source_count)
{
   static const char *note =
       "\n\n# Source Authority\n"
       "Aimee index, code_search, find_symbol, and search_memory are authoritative for discovery, "
       "routing, and prior context. Current source is authoritative for file contents: any "
       "source packet in this prompt, explicitly preloaded file, or read_file result from the "
       "delegate worktree overrides indexed snippets when they differ. If an index-backed tool "
       "marks a result stale or discovery_only, use it as a lead and inspect the current source "
       "before editing or making a content claim.\n";
   if (!base || !base[0] || source_count < 0)
      return NULL;
   size_t blen = strlen(base);
   size_t nlen = strlen(note);
   char *out = malloc(blen + nlen + 1);
   if (!out)
      return NULL;
   memcpy(out, base, blen);
   memcpy(out + blen, note, nlen + 1);
   return out;
}

void delegate_source_env_capture(delegate_source_env_snapshot_t *snap)
{
   if (!snap)
      return;
   memset(snap, 0, sizeof(*snap));
   const char *source_auth = getenv("AIMEE_DELEGATE_SOURCE_AUTHORITY");
   const char *source_paths = getenv("AIMEE_DELEGATE_SOURCE_PATHS");
   const char *worktree_root = getenv("AIMEE_DELEGATE_WORKTREE_ROOT");
   if (source_auth && source_auth[0])
      snprintf(snap->source_auth, sizeof(snap->source_auth), "%s", source_auth);
   if (source_paths && source_paths[0])
      snprintf(snap->source_paths, sizeof(snap->source_paths), "%s", source_paths);
   if (worktree_root && worktree_root[0])
      snprintf(snap->worktree_root, sizeof(snap->worktree_root), "%s", worktree_root);
}

void delegate_source_env_restore(const delegate_source_env_snapshot_t *snap)
{
   if (!snap)
      return;
   platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", snap->source_auth);
   platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", snap->source_paths);
   platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", snap->worktree_root);
}

void delegate_source_env_clear_for_worktree(const char *worktree_root)
{
   platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", worktree_root ? worktree_root : "");
   platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "");
   platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", "");
}
