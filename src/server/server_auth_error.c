/* server_auth_error.c: the JSON body for a /v1 auth rejection.
 *
 * Its own translation unit because server_http.c sits at the 2500-line
 * ceiling enforced by line-check; this is small, pure, and independently
 * testable, so it is the right thing to lift out rather than raising the cap.
 */
#include "server_http.h"

/* Body for an auth rejection on /v1. Split out of handle_conn so the wording is
 * testable — the remediation below is the whole point of the message and had no
 * coverage.
 *
 * A previously-enrolled client that starts failing with 401 has almost always
 * been invalidated by a bearer rotation, not by a typo: rotate_bearer replaces
 * the server-wide bearer wholesale, so every already-paired client breaks at
 * once. The bare "missing or invalid bearer token" gave no way to tell that
 * apart from a bad token, and no way back — recovering meant knowing to read
 * aimee.yaml inside the container. Name the recovery path here, where every
 * client sees it (the thin client echoes this text verbatim). */
const char *server_http_auth_error_body(int az)
{
   if (az == 401)
      return "{\"error\":{\"message\":\"missing or invalid bearer token. If this client was "
             "working before, the server's bearer has been rotated and every paired client must "
             "be re-pointed at the new one: read aimee.api.bearer_token from "
             "<AIMEE_HOME>/aimee.yaml on the server, then `aimee remote set <url> <token>`\","
             "\"type\":\"authentication_error\"}}";
   return "{\"error\":{\"message\":\"this endpoint requires a configured bearer token\","
          "\"type\":\"server_error\"}}";
}
