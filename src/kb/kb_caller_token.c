#include "kb_caller_token.h"

#include <string.h>

server_identity_token_result_t
kb_caller_token_verify(const char *jwt, size_t jwt_len, const char *jwks_json,
                       const char *server_id, int64_t named_team, int64_t now,
                       server_identity_token_claims_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!jwt || !jwt_len || !jwks_json || !server_id || !server_id[0] || named_team <= 0 || !out)
      return SERVER_IDENTITY_TOKEN_INVALID;

   server_identity_token_result_t result =
       server_identity_token_verify(jwt, jwt_len, jwks_json, "kb", server_id, now, out);
   if (result != SERVER_IDENTITY_TOKEN_OK || out->team_id != named_team ||
       strncmp(out->subject, "oidc:", 5) != 0)
   {
      memset(out, 0, sizeof(*out));
      return SERVER_IDENTITY_TOKEN_INVALID;
   }
   return SERVER_IDENTITY_TOKEN_OK;
}
