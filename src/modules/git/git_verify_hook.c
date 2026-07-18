#include "aimee.h"
#include "git_verify.h"
#include "platform_path.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int verify_install_git_hook(const char *project_root)
{
   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --git-common-dir 2>/dev/null",
               project_root);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse --git-common-dir 2>/dev/null");

   int rc;
   char *gitdir_raw = run_cmd(cmd, &rc);
   if (rc != 0 || !gitdir_raw || !gitdir_raw[0])
   {
      free(gitdir_raw);
      return -1;
   }

   char *nl = gitdir_raw + strlen(gitdir_raw) - 1;
   while (nl >= gitdir_raw && (*nl == '\n' || *nl == '\r' || *nl == ' '))
      *nl-- = '\0';

   char hooks_dir[MAX_PATH_LEN];
   if (gitdir_raw[0] == '/')
   {
      snprintf(hooks_dir, sizeof(hooks_dir), "%s/hooks", gitdir_raw);
   }
   else
   {
      char base[MAX_PATH_LEN];
      if (project_root && project_root[0])
         snprintf(base, sizeof(base), "%s", project_root);
      else if (!getcwd(base, sizeof(base)))
      {
         free(gitdir_raw);
         return -1;
      }
      snprintf(hooks_dir, sizeof(hooks_dir), "%s/%s/hooks", base, gitdir_raw);
   }
   free(gitdir_raw);

   if (platform_mkdir_p(hooks_dir, 0755) != 0 && errno != EEXIST)
      return -1;

   char hook_path[MAX_PATH_LEN];
   snprintf(hook_path, sizeof(hook_path), "%s/pre-push", hooks_dir);

   struct stat st;
   if (stat(hook_path, &st) == 0)
   {
      FILE *fcheck = fopen(hook_path, "r");
      if (fcheck)
      {
         char buf[256];
         int is_ours = 0;
         while (fgets(buf, sizeof(buf), fcheck))
         {
            if (strstr(buf, "installed by aimee"))
            {
               is_ours = 1;
               break;
            }
         }
         fclose(fcheck);
         if (!is_ours)
            return -2;
      }
   }

   FILE *f = fopen(hook_path, "w");
   if (!f)
      return -1;

   fprintf(f, "#!/bin/sh\n");
   fprintf(f, "# pre-push hook -- installed by aimee (aimee git verify action=install-hook)\n");
   fprintf(f, "# Blocks push when the last verify run has failed steps or is stale.\n");
   fprintf(f, "# To bypass once: git push --no-verify\n");
   fprintf(f, "if command -v aimee >/dev/null 2>&1; then\n");
   fprintf(f, "    # Read each ref being pushed from stdin (format: local_ref local_sha remote_ref "
              "remote_sha).\n");
   fprintf(f, "    # Pass the local SHA so verify_check can match against the exact commit\n");
   fprintf(f, "    # that was verified in this checkout.\n");
   fprintf(f, "    while IFS=' ' read -r local_ref local_sha remote_ref remote_sha; do\n");
   fprintf(f, "        result=$(aimee git verify action=check commit=\"$local_sha\" "
              "2>/dev/null)\n");
   fprintf(f, "        if printf '%%s\\n' \"$result\" | grep -q '^FAIL'; then\n");
   fprintf(f, "            printf 'aimee: push blocked -- %%s\\n' \"$result\" >&2\n");
   fprintf(f, "            printf \"aimee: run 'aimee git verify' to re-verify\\n\" >&2\n");
   fprintf(f, "            exit 1\n");
   fprintf(f, "        fi\n");
   fprintf(f, "    done\n");
   fprintf(f, "fi\n");
   fclose(f);

   chmod(hook_path, 0755);
   return 0;
}
