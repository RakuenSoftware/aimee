#ifndef PLATFORM_TEST_UTIL_H
#define PLATFORM_TEST_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef AIMEE_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>
#include <direct.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

static inline const char *platform_tmpdir(void)
{
   static char buf[MAX_PATH];
   DWORD len = GetTempPathA(MAX_PATH, buf);
   if (len == 0 || len >= MAX_PATH)
   {
      return ".";
   }
   if (len > 0 && (buf[len - 1] == '\\' || buf[len - 1] == '/'))
   {
      buf[len - 1] = '\0';
   }
   return buf;
}

#ifndef DEC_PLATFORM_PROCESS_H
static inline int platform_setenv(const char *name, const char *value)
{
   return SetEnvironmentVariableA(name, value) ? 0 : -1;
}

static inline int platform_unsetenv(const char *name)
{
   return SetEnvironmentVariableA(name, NULL) ? 0 : -1;
}
#endif

static inline char *platform_mkdtemp(char *tmpl)
{
   char unique[MAX_PATH];
   UINT rc = GetTempFileNameA(platform_tmpdir(), "aim", 0, unique);
   if (rc == 0)
   {
      return NULL;
   }
   DeleteFileA(unique);
   if (_mkdir(unique) != 0)
   {
      return NULL;
   }
   strncpy(tmpl, unique, PATH_MAX - 1);
   tmpl[PATH_MAX - 1] = '\0';
   return tmpl;
}

static inline int platform_test_mkdir(const char *path, int mode)
{
   (void)mode;
   return _mkdir(path);
}

static inline int platform_mkstemp(char *path, size_t path_size, const char *prefix)
{
   char filename[MAX_PATH];
   UINT rc = GetTempFileNameA(platform_tmpdir(), prefix ? prefix : "aim", 0, filename);
   int fd;
   if (rc == 0)
   {
      return -1;
   }
   strncpy(path, filename, path_size - 1);
   path[path_size - 1] = '\0';
   fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
   if (fd < 0)
   {
      DeleteFileA(path);
   }
   return fd;
}
#else
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* Honour TMPDIR so the suite can confine every test's scratch space to one
 * directory it already reaps.
 *
 * This returned a hardcoded "/tmp", which made the leak below unfixable from
 * the harness: the unit-tests target creates a throwaway HOME and removes it on
 * EXIT, but 64 of the 112 tests that call platform_mkdtemp() never remove their
 * own directory, and every one of them landed in /tmp regardless of that trap.
 * They accumulated across runs until tmpfs ran out of INODES — 40k leaked
 * directories holding 857k inodes, 82% of the filesystem, with 45GB of space
 * still free. At that point nothing on the machine could create a file.
 *
 * With TMPDIR respected, those directories are created inside the run's own
 * temp home and the existing trap takes them with it. The 64 tests are left
 * alone deliberately: one honoured variable fixes them all, and fixing them
 * individually would be 64 chances to miss one. */
static inline const char *platform_tmpdir(void)
{
   const char *dir = getenv("TMPDIR");
   return (dir && dir[0] == '/') ? dir : "/tmp";
}
#ifndef DEC_PLATFORM_PROCESS_H
static inline int platform_setenv(const char *name, const char *value)
{
   return setenv(name, value, 1);
}
static inline int platform_unsetenv(const char *name)
{
   return unsetenv(name);
}
#endif
static inline char *platform_mkdtemp(char *tmpl)
{
   return mkdtemp(tmpl);
}
static inline int platform_test_mkdir(const char *path, int mode)
{
   return mkdir(path, (mode_t)mode);
}
static inline int platform_mkstemp(char *path, size_t path_size, const char *prefix)
{
   (void)path_size;
   (void)prefix;
   return mkstemp(path);
}
#endif

#ifndef unplatform_setenv
#define unplatform_setenv platform_unsetenv
#endif

/* Remove a SQLite db file plus its WAL, SHM, and journal siblings.
 * Tests routinely call db_open() / db1_init() on a tmp path; without
 * this, the -wal and -shm files SQLite creates leak in tmpfs and pile
 * up across test runs. Safe to call with a non-existent path. */
static inline void platform_test_remove_sqlite(const char *path)
{
   if (!path || !path[0])
      return;
   char buf[1024];
#ifdef AIMEE_WINDOWS
   DeleteFileA(path);
   snprintf(buf, sizeof(buf), "%s-wal", path);
   DeleteFileA(buf);
   snprintf(buf, sizeof(buf), "%s-shm", path);
   DeleteFileA(buf);
   snprintf(buf, sizeof(buf), "%s-journal", path);
   DeleteFileA(buf);
#else
   (void)remove(path);
   snprintf(buf, sizeof(buf), "%s-wal", path);
   (void)remove(buf);
   snprintf(buf, sizeof(buf), "%s-shm", path);
   (void)remove(buf);
   snprintf(buf, sizeof(buf), "%s-journal", path);
   (void)remove(buf);
#endif
}

/* Recursively remove a directory created by platform_mkdtemp.
 * Refuses to operate on paths that don't look like temp dirs (must
 * contain "aimee-") to limit blast radius if a test passes a wrong
 * path. Safe to call with a non-existent path. */
static inline void platform_test_rmrf(const char *path)
{
   if (!path || !path[0])
      return;
   if (!strstr(path, "aimee-"))
      return;
   char cmd[1280];
#ifdef AIMEE_WINDOWS
   snprintf(cmd, sizeof(cmd), "rmdir /S /Q \"%s\" 2>NUL", path);
#else
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null", path);
#endif
   (void)!system(cmd);
}

#endif /* PLATFORM_TEST_UTIL_H */
