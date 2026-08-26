/* appliance_admin.c: one source of truth for the browser operator identity.
 * runtime-web creates these records while replacing the generated first-boot
 * login. Server-side policy gates must read them in the same order or the UI can
 * offer an action that the resource plane then refuses. */
#include "appliance_admin.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

void appliance_admin_webuser(char *out, size_t out_n)
{
   if (!out || !out_n)
      return;
   out[0] = '\0';
   const char *home = config_default_dir();
   if (!home || !home[0])
   {
      snprintf(out, out_n, "admin");
      return;
   }
   const char *records[] = {"bootstrap-replaced", "bootstrap-user"};
   for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++)
   {
      char path[4096];
      if ((size_t)snprintf(path, sizeof(path), "%s/webchat/%s", home, records[i]) >= sizeof(path))
         continue;
      FILE *f = fopen(path, "r");
      if (!f)
         continue;
      char line[256] = "";
      if (!fgets(line, sizeof(line), f))
      {
         fclose(f);
         continue;
      }
      fclose(f);
      line[strcspn(line, "\r\n")] = '\0';
      /* bootstrap-user is "<explicit|generated>:<name>";
       * bootstrap-replaced contains the bare replacement name. */
      const char *name = strchr(line, ':');
      name = name ? name + 1 : line;
      while (*name == ' ')
         name++;
      if (*name)
      {
         snprintf(out, out_n, "%s", name);
         return;
      }
   }
   /* Preserve the historical closed default when no onboarding record exists. */
   snprintf(out, out_n, "admin");
}

int appliance_admin_principal_authorized(const char *principal)
{
   static const char prefix[] = "webuser:";
   if (!principal || strncmp(principal, prefix, sizeof(prefix) - 1) != 0 ||
       !principal[sizeof(prefix) - 1])
      return 0;
   char admin[128];
   appliance_admin_webuser(admin, sizeof(admin));
   return admin[0] && strcmp(principal + sizeof(prefix) - 1, admin) == 0;
}
