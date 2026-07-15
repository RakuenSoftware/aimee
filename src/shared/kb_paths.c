/* kb_paths.c: shared path helpers for aimee-server and aimee-kb. */
#include "kb_paths.h"
#include "aimee_home.h"
#include <stdio.h>
#include <string.h>

const char *kb_default_config_dir(void)
{
   static char dir[4096];
   if (dir[0])
      return dir;
   const char *base = aimee_home();
   if (!base)
      base = "/tmp/.config/aimee";
   snprintf(dir, sizeof(dir), "%s", base);
   return dir;
}

const char *kb_default_socket_path(void)
{
   static char path[4096];
   if (path[0])
      return path;
   snprintf(path, sizeof(path), "%s/%s", kb_default_config_dir(), "aimee-kb.sock");
   return path;
}

const char *kb_default_bg_socket_path(void)
{
   static char path[4096];
   if (path[0])
      return path;
   const char *base = kb_default_socket_path();
   const char *dot = strrchr(base, '.');
   if (dot && strcmp(dot, ".sock") == 0)
      snprintf(path, sizeof(path), "%.*s-bg.sock", (int)(dot - base), base);
   else
      snprintf(path, sizeof(path), "%s-bg.sock", base);
   return path;
}
