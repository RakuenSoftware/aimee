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
