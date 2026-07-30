/* Container Vault bootstrap privilege boundary.
 *
 * Older images created AIMEE_HOME/.vault as root even though the files inside
 * belonged to the unprivileged runtime account. Repair only that closed, flat
 * directory before credentials are touched, then irrevocably drop privilege. */
#include "vault_bootstrap_privilege.h"
#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int vault_bootstrap_parse_args(int argc, char **argv, const char **drop_user)
{
   if (!argv || !drop_user || argc < 2 || !argv[1] ||
       strcmp(argv[1], "--bootstrap-vault-env") != 0)
      return -1;
   *drop_user = NULL;
   if (argc == 2)
      return 0;
   if (argc == 4 && argv[2] && argv[3] && argv[3][0] &&
       strcmp(argv[2], "--drop-user") == 0)
   {
      *drop_user = argv[3];
      return 0;
   }
   return -1;
}

#ifdef AIMEE_WINDOWS

int vault_bootstrap_run_as(const char *user)
{
   /* Container privilege dropping is a POSIX-only startup contract. */
   return user && user[0] ? -1 : 0;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void close_files(int *fds, size_t count)
{
   for (size_t i = 0; i < count; i++)
      close(fds[i]);
   free(fds);
}

int vault_bootstrap_repair_owner_at(const char *home, uid_t uid, gid_t gid)
{
   static const char suffix[] = "/.vault";
   if (!home || !home[0])
      return -1;
   size_t home_len = strlen(home);
   if (home_len > SIZE_MAX - sizeof(suffix))
      return -1;
   char *path = malloc(home_len + sizeof(suffix));
   if (!path)
      return -1;
   memcpy(path, home, home_len);
   memcpy(path + home_len, suffix, sizeof(suffix));

   int dir_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   free(path);
   if (dir_fd < 0)
      return errno == ENOENT ? 0 : -1;

   int rc = -1;
   int *files = NULL;
   size_t file_count = 0;
   size_t file_cap = 0;
   struct stat dir_st;
   if (fstat(dir_fd, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) ||
       (dir_st.st_uid != 0 && dir_st.st_uid != uid))
      goto done;

   /* fdopendir owns its descriptor, so retain dir_fd for pinned openat calls. */
   int scan_fd = dup(dir_fd);
   if (scan_fd < 0)
      goto done;
   DIR *dir = fdopendir(scan_fd);
   if (!dir)
   {
      close(scan_fd);
      goto done;
   }

   for (;;)
   {
      errno = 0;
      struct dirent *entry = readdir(dir);
      if (!entry)
      {
         if (errno != 0)
         {
            closedir(dir);
            goto done;
         }
         break;
      }
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         continue;

      /* O_NONBLOCK prevents a hostile FIFO from stalling privileged startup.
       * The returned fd pins the inode: rename cannot redirect later changes. */
      int fd = openat(dir_fd, entry->d_name,
                      O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
      struct stat st;
      if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
          (st.st_uid != 0 && st.st_uid != uid))
      {
         if (fd >= 0)
            close(fd);
         closedir(dir);
         goto done;
      }
      if (file_count == file_cap)
      {
         size_t next_cap = file_cap ? file_cap * 2 : 8;
         if (next_cap < file_cap || next_cap > SIZE_MAX / sizeof(*files))
         {
            close(fd);
            closedir(dir);
            goto done;
         }
         int *next = realloc(files, next_cap * sizeof(*files));
         if (!next)
         {
            close(fd);
            closedir(dir);
            goto done;
         }
         files = next;
         file_cap = next_cap;
      }
      files[file_count++] = fd;
   }
   closedir(dir);

   /* Validate and pin every child before mutating any of them. */
   for (size_t i = 0; i < file_count; i++)
   {
      struct stat st;
      if (fstat(files[i], &st) != 0 || !S_ISREG(st.st_mode) ||
          (st.st_uid != 0 && st.st_uid != uid) ||
          (st.st_uid == 0 && fchown(files[i], uid, gid) != 0) ||
          fchmod(files[i], 0600) != 0)
         goto done;
   }
   if (((dir_st.st_uid != uid || dir_st.st_gid != gid) &&
        fchown(dir_fd, uid, gid) != 0) ||
       fchmod(dir_fd, 0700) != 0)
      goto done;
   rc = 0;

done:
   close_files(files, file_count);
   close(dir_fd);
   return rc;
}

int vault_bootstrap_run_as(const char *user)
{
   if (!user || !user[0])
      return geteuid() == 0 ? -1 : 0;

   errno = 0;
   struct passwd *pw = getpwnam(user);
   if (!pw)
   {
      fprintf(stderr, "aimee-server: bootstrap user '%s' lookup failed: %s\n",
              user, strerror(errno ? errno : ENOENT));
      return -1;
   }
   if (pw->pw_uid == 0)
   {
      fprintf(stderr, "aimee-server: bootstrap user '%s' must not be root\n", user);
      return -1;
   }
   uid_t uid = pw->pw_uid;
   gid_t gid = pw->pw_gid;
   if (geteuid() == uid)
      return getegid() == gid ? 0 : -1;
   if (geteuid() != 0)
   {
      fprintf(stderr, "aimee-server: cannot drop from uid %lu to bootstrap user '%s'\n",
              (unsigned long)geteuid(), user);
      return -1;
   }

   /* Repair MUST run as root. setuid below drops saved privilege as well, and
    * every subsequent credential operation must execute as the target user. */
   if (vault_bootstrap_repair_owner_at(config_default_dir(), uid, gid) != 0 ||
       initgroups(user, gid) != 0 || setgid(gid) != 0 || setuid(uid) != 0)
      return -1;
   return geteuid() == uid && getegid() == gid ? 0 : -1;
}

#endif
