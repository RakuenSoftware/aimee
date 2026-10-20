/* platform_path.c: Windows path and filesystem utilities */
#include "platform_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <windows.h>

const char *platform_home_dir(void)
{
   static char path[MAX_PATH];
   DWORD rc = GetEnvironmentVariableA("USERPROFILE", path, sizeof(path));
   return (rc > 0 && rc < sizeof(path)) ? path : NULL;
}

const char *platform_config_dir(void)
{
   static char path[MAX_PATH];
   DWORD rc = GetEnvironmentVariableA("APPDATA", path, sizeof(path));
   if (rc == 0 || rc >= sizeof(path))
      return NULL;
   snprintf(path + strlen(path), sizeof(path) - strlen(path), "\\aimee");
   return path;
}

char platform_path_sep(void)
{
   return '\\';
}

static char *resolved_handle_path(const char *path, int directory)
{
   HANDLE handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
   if (handle == INVALID_HANDLE_VALUE)
      return NULL;
   BY_HANDLE_FILE_INFORMATION info;
   if (directory && (!GetFileInformationByHandle(handle, &info) ||
                     !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
   {
      CloseHandle(handle);
      return NULL;
   }
   DWORD length = GetFinalPathNameByHandleA(handle, NULL, 0, VOLUME_NAME_NT);
   char *resolved = length && length < 65536 ? malloc((size_t)length + 1) : NULL;
   if (resolved)
   {
      DWORD written = GetFinalPathNameByHandleA(handle, resolved, length + 1, VOLUME_NAME_NT);
      if (!written || written > length)
      {
         free(resolved);
         resolved = NULL;
      }
   }
   CloseHandle(handle);
   return resolved;
}

int platform_path_is_beneath(const char *root, const char *path)
{
   if (!root || !root[0] || !path || !path[0])
      return 0;
   char *parent = resolved_handle_path(root, 1), *child = resolved_handle_path(path, 0);
   int within = 0;
   if (parent && child)
   {
      size_t length = strlen(parent);
      within = !strncmp(parent, child, length) &&
               (parent[length - 1] == '\\' || child[length] == '\\' || !child[length]);
   }
   free(parent);
   free(child);
   return within;
}

int platform_mkdir_p(const char *path, int mode)
{
   (void)mode;
   char tmp[MAX_PATH];
   size_t len;
   if (!path)
      return -1;
   snprintf(tmp, sizeof(tmp), "%s", path);
   len = strlen(tmp);
   if (len == 0)
      return -1;
   if (tmp[len - 1] == '\\' || tmp[len - 1] == '/')
      tmp[len - 1] = '\0';

   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '\\' || *p == '/')
      {
         char saved = *p;
         *p = '\0';
         if (strlen(tmp) > 0 && !CreateDirectoryA(tmp, NULL) &&
             GetLastError() != ERROR_ALREADY_EXISTS)
            return -1;
         *p = saved;
      }
   }
   if (!CreateDirectoryA(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
      return -1;
   return 0;
}

int platform_unlink(const char *path)
{
   return DeleteFileA(path) ? 0 : -1;
}

int platform_set_permissions(const char *path, int mode)
{
   (void)path;
   (void)mode;
   return 0;
}

int platform_getppid(void)
{
   DWORD self = GetCurrentProcessId();
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   if (snap == INVALID_HANDLE_VALUE)
      return -1;

   PROCESSENTRY32 pe;
   pe.dwSize = sizeof(pe);
   if (!Process32First(snap, &pe))
   {
      CloseHandle(snap);
      return -1;
   }

   do
   {
      if (pe.th32ProcessID == self)
      {
         CloseHandle(snap);
         return (int)pe.th32ParentProcessID;
      }
   } while (Process32Next(snap, &pe));

   CloseHandle(snap);
   return -1;
}
