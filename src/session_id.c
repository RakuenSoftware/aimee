#include "config.h"
#include "platform_path.h"
#include "platform_process.h"
#include "platform_random.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static _Thread_local char g_session_override[64];
static _Thread_local int g_session_drop;

void session_id_refresh(void)
{
   if (!g_session_override[0])
      g_session_drop = 1;
}

const char *session_id(void)
{
   static _Thread_local char id[64];
   if (g_session_override[0])
      return g_session_override;
   if (g_session_drop)
      id[0] = 0, g_session_drop = 0;
   if (id[0])
      return id;
   int ppid = (int)platform_getppid();
   char path[MAX_PATH_LEN];
   if (ppid > 1)
   {
      snprintf(path, sizeof(path), "%s/session-ppid-%d", config_default_dir(), ppid);
      FILE *file = fopen(path, "r");
      if (file)
      {
         if (fgets(id, sizeof(id), file))
            id[strcspn(id, "\r\n ")] = 0;
         fclose(file);
         if (id[0])
            return id;
      }
   }
   unsigned char random[16];
   if (platform_random_bytes(random, sizeof(random)) != 0)
      memset(random, 0, sizeof(random));
   snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            random[0], random[1], random[2], random[3], random[4], random[5], random[6], random[7],
            random[8], random[9], random[10], random[11], random[12], random[13], random[14],
            random[15]);
   if (ppid > 1)
   {
      (void)platform_mkdir_p(config_default_dir(), 0700);
      snprintf(path, sizeof(path), "%s/session-ppid-%d", config_default_dir(), ppid);
      int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (fd >= 0)
      {
         (void)write(fd, id, strlen(id));
         close(fd);
      }
   }
   return id;
}

void session_id_set_override(const char *sid)
{
   snprintf(g_session_override, sizeof(g_session_override), "%s", sid ? sid : "");
}

void session_id_clear_override(void)
{
   g_session_override[0] = 0;
}

int session_id_override_active(void)
{
   return g_session_override[0] != 0;
}
