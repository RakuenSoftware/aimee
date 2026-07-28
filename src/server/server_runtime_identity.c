#include "server_runtime_identity.h"

#include "kb_client_mtls.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int server_id_valid(const char *value, char *out, size_t cap)
{
   size_t n = value ? strlen(value) : 0;
   if (!n || n >= cap || n > 127)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!(isalnum((unsigned char)value[i]) || strchr("._-", value[i])))
         return 0;
   memcpy(out, value, n + 1);
   return 1;
}

static int team_id_valid(const char *value, int64_t *out)
{
   if (!value || !value[0] || !out)
      return 0;
   char *end = NULL;
   errno = 0;
   long long parsed = strtoll(value, &end, 10);
   if (errno || !end || *end || parsed <= 0)
      return 0;
   *out = (int64_t)parsed;
   return 1;
}

server_runtime_identity_state_t server_runtime_identity_load(char *server_id, size_t server_id_cap,
                                                             int64_t *team_id)
{
   if (server_id && server_id_cap)
      server_id[0] = '\0';
   if (team_id)
      *team_id = 0;
   if (!server_id || server_id_cap < 2 || !team_id)
      return SERVER_RUNTIME_IDENTITY_NO_TEAM;

   const char *explicit_server = getenv("AIMEE_SERVER_ID");
   const char *explicit_team = getenv("AIMEE_SERVER_TEAM_ID");
   const int has_server = explicit_server && explicit_server[0];
   const int has_team = explicit_team && explicit_team[0];
   if (has_server || has_team)
   {
      if (!team_id_valid(explicit_team, team_id))
         return SERVER_RUNTIME_IDENTITY_NO_TEAM;
      if (!server_id_valid(explicit_server, server_id, server_id_cap))
      {
         *team_id = 0;
         return SERVER_RUNTIME_IDENTITY_NO_SERVER_ID;
      }
      return SERVER_RUNTIME_IDENTITY_READY;
   }

   long long managed_team = 0;
   char managed_server[128];
   if (!kb_client_mtls_managed_metadata(managed_server, sizeof(managed_server), &managed_team) ||
       managed_team <= 0 || !server_id_valid(managed_server, server_id, server_id_cap))
      return SERVER_RUNTIME_IDENTITY_NO_TEAM;
   *team_id = (int64_t)managed_team;
   return SERVER_RUNTIME_IDENTITY_READY;
}

int server_runtime_server_id_load(char *server_id, size_t server_id_cap)
{
   if (server_id && server_id_cap)
      server_id[0] = '\0';
   if (!server_id || server_id_cap < 2)
      return 0;
   const char *explicit_server = getenv("AIMEE_SERVER_ID");
   const char *explicit_team = getenv("AIMEE_SERVER_TEAM_ID");
   if (explicit_server && explicit_server[0])
      return server_id_valid(explicit_server, server_id, server_id_cap);
   /* A non-empty explicit team with no explicit server is a partial packet;
    * never fill it from managed state. */
   if (explicit_team && explicit_team[0])
      return 0;
   long long managed_team = 0;
   char managed_server[128];
   return kb_client_mtls_managed_metadata(managed_server, sizeof(managed_server), &managed_team) &&
                  managed_team > 0 && server_id_valid(managed_server, server_id, server_id_cap)
              ? 1
              : 0;
}
