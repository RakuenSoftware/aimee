/* server_http_authz.c: who may call which /v1 route.
 *
 * Split out of server_http.c, which was over its 2500-line limit after the
 * per-user write-tier work landed — the same reason server_http_identity.c was
 * split out before it. This is not an arbitrary slice: everything here answers
 * one question, "is this caller allowed to do this?", and none of it touches a
 * socket except to read the request's Authorization header.
 *
 * Three layers, in the order handle_conn applies them:
 *
 *   1. Connection capabilities — what the transport and credential grant
 *      (server_http_conn_caps / server_http_effective_conn_caps). UDS is the
 *      same-user trusted peer and gets CAPS_ALL.
 *   2. The per-user write tier — resolved from the caller's kb-signed identity
 *      token by server_http_resolve_write_tier (proposal
 *      per-user-remote-writes-authz.md §5). TCP only.
 *   3. The route gate — server_http_route_allowed_caps, which needs both the
 *      capability bitmask and that tier.
 *
 * The retired aimee.api.remote_writes counter lives here too, because the thing
 * it counts is a decision made in this file.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strncasecmp via strings.h, matching server_http.c */
#endif
#include "server_http_internal.h"
#include "server_http.h"
#include "server_http_authz.h"

#include "kb_identity_token.h" /* KB_IDENTITY_TOKEN_WIRE_MAX */
#include "log.h"
#include "server.h" /* CAP_* / CAPS_* */
#include "server_write_tier.h"
#include "server_write_tier_db1.h" /* the db1-backed verify/consume wrappers */
#include <aimee/core/connection/auth.h>

#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* 1 if the route is a data-plane write. At the default remote_writes=off these
 * routes are local-UDS-only; remote_writes=data/full can expose them over TCP
 * after the per-route capability check. */

/* Capability bitmask a /v1 route requires (route_caps subset of conn_caps gates
 * the request in handle_conn). 0 = public, or an unrecognized route (which then
 * 404s in the router). Pure — no socket, no globals. */
uint32_t server_http_route_caps(const char *method, const char *path)
{
   return v1_route_caps_lookup(method, path);
}

/* Public accessor for the data-write classification (test + introspection).
 * Historical name retained for ABI compatibility; see
 * v1_route_is_local_only in server_http_routes.inc. */
int server_http_route_is_local_only(const char *method, const char *path)
{
   return v1_route_is_local_only(method, path);
}

uint32_t server_http_conn_caps(int is_tcp, const char *bearer, int remote_writes)
{
   if (!is_tcp)
      return CAPS_ALL; /* UDS: same-user, filesystem-attested */
   if (bearer && strncmp(bearer, "scope:", 6) == 0)
      return CAPS_READ_ONLY & ~(uint32_t)CAP_CHAT; /* scoped: query-only, no compute */
   /* Unscoped TCP bearer. "full" makes it fully trusted (CAPS_ALL), which also
    * permits the delegate/tool methods over /v1 (gated on == CAPS_ALL); "off"/"data"
    * keep CAPS_AUTHENTICATED (write caps present, but mutating routes are gated
    * separately in server_http_route_allowed). */
   if (remote_writes >= SERVER_REMOTE_WRITES_FULL)
      return CAPS_ALL;
   return CAPS_AUTHENTICATED;
}

uint32_t server_http_effective_conn_caps(int is_tcp, const char *bearer, int remote_writes,
                                         int mtls_mode, int mtls_authenticated)
{
   if (!is_tcp || mtls_mode <= 0)
      return server_http_conn_caps(is_tcp, bearer, remote_writes);
   if (mtls_authenticated)
      /* `remote_writes` is the caller's verified per-user tier by this point,
       * not the retired process-global switch. Preserve the ordinary mTLS
       * authenticated floor for off/data, but a certificate-bound full grant
       * must expose the same full capability set as any other verified full
       * identity. Otherwise CAP_INDEX_ADMIN routes such as kb.build remain
       * impossible even for the browser wizard's enrolled owner. */
      return remote_writes >= SERVER_REMOTE_WRITES_FULL ? CAPS_ALL : CAPS_AUTHENTICATED;
   /* Optional-mode bearer fallback is deliberately weaker than a client cert:
    * query/session reads only, with no compute or mutation capability. */
   return CAPS_READ_ONLY & ~(uint32_t)CAP_CHAT;
}

int server_http_mtls_transport_allowed(int is_tcp, int mtls_mode, int mtls_authenticated,
                                       const char *method, const char *path)
{
   if (!is_tcp || mtls_mode < 2 || mtls_authenticated)
      return 1;
   /* Required posture is enforced here instead of with
    * SSL_VERIFY_FAIL_IF_NO_PEER_CERT so a new client can reach the enrollment
    * handlers. These are transport exceptions only: the ordinary bearer,
    * bootstrap, capability and single-use binding gates still run below. */
   if (!method || !path || strcmp(method, "POST") != 0)
      return 0;
   return strcmp(path, "/v1/api/rotate_bearer") == 0 ||
          strcmp(path, "/v1/api/enroll_bearer") == 0 || strcmp(path, "/v1/cert/sign") == 0;
}

/* Routes deliberately reachable over the TCP listener regardless of
 * aimee.api.remote_writes (still capability-gated): the detached-workspace
 * reverse channel and registry mutations (workspace-resource-plane). The serving
 * client IS the fs/exec authority and must drive these from another host. */
static int v1_route_tcp_exempt(const char *method, const char *path)
{
   if (!method || !path)
      return 0;
   if (strcmp(path, "/v1/runner/poll") == 0 || strcmp(path, "/v1/runner/respond") == 0)
      return 1;
   if (strcmp(method, "POST") == 0 &&
       (strcmp(path, "/v1/api/rotate_bearer") == 0 || strcmp(path, "/v1/api/enroll_bearer") == 0 ||
        strcmp(path, "/v1/cert/sign") == 0))
      return 1;
   if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/workspaces") == 0) /* workspace.add */
      return 1;
   /* Same plane, same reason: the remote fs authority ships its working-tree
    * patch here so the mirror reconstructs the tree it actually has. This route
    * needs tool:execute, so without the exemption it would demand
    * remote_writes=full over TCP — i.e. at the default a thin client could never
    * upload, and the server would reconstruct a clean checkout at head with the
    * client's uncommitted work silently missing. */
   if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/workspace/mirror-sync") == 0)
      return 1;
   if (strcmp(method, "DELETE") == 0 && strncmp(path, "/v1/workspaces/", 15) == 0) /* .remove */
      return 1;
   return 0;
}

/* No /v1 route is UDS-only any more.
 *
 * The family this guarded — /v1/grants/write-tier — is gone: aimee-server no
 * longer proxies write-tier grant administration. That is an operator action
 * against aimee-kb, where the DB layer's admin-or-team-lead RLS check is the
 * authority. Proxying it meant aimee-server needed an administrative identity on
 * aimee-kb, which is precisely what a single-tenant data-plane service should
 * not hold. Kept as a seam (returning 0) rather than deleted, so a future
 * operator-only route has somewhere obvious to declare itself. */
int v1_route_requires_uds(const char *method, const char *path)
{
   (void)method;
   (void)path;
   return 0;
}

int server_http_route_allowed_caps(int is_tcp, uint32_t have, const char *method, const char *path,
                                   int remote_writes)
{
   /* Checked FIRST and unconditionally: this must not be reachable by satisfying a
    * capability or a tier, so it sits ahead of both gates rather than inside them. */
   if (is_tcp && v1_route_requires_uds(method, path))
      return 0;
   /* Over the TCP listener, a route that needs any capability beyond the read set
    * (CAPS_READ_ONLY) is "privileged" and denied unless the operator opts in via
    * aimee.api.remote_writes, so a leaked/shared bearer cannot mutate or execute
    * remotely at the default. Two tiers: data-plane writes (memory, work, rules,
    * skill, ... — v1_route_is_local_only / g_v1_write_ops) need
    * remote_writes>=data; everything else privileged (delegate, cron, agent,
    * provider, api, worktree, session admin, ...) is exec/control and needs
    * remote_writes==full. The detached-workspace plane (runner + workspace
    * add/remove) is exempt — designed to be driven by a remote fs authority. UDS
    * is the same-user trusted peer and bypasses all of this. */
   if (is_tcp && !v1_route_tcp_exempt(method, path))
   {
      int data_write = v1_route_is_local_only(method, path); /* g_v1_write_ops */
      uint32_t rc = server_http_route_caps(method, path);
      int privileged = (rc & ~(uint32_t)CAPS_READ_ONLY) != 0; /* needs a non-read cap */
      if (data_write || privileged)
      {
         /* data-plane writes open at "data"; everything else privileged (exec/
          * control) needs "full". A data-write keeps the lower bar even if its
          * cap is weak. */
         int need_tier = data_write ? SERVER_REMOTE_WRITES_DATA : SERVER_REMOTE_WRITES_FULL;
         if (remote_writes < need_tier)
            return 0;
      }
   }
   uint32_t need = server_http_route_caps(method, path);
   return (need & ~have) == 0;
}

int server_http_route_allowed(int is_tcp, const char *bearer, const char *method, const char *path,
                              int remote_writes)
{
   return server_http_route_allowed_caps(
       is_tcp, server_http_conn_caps(is_tcp, bearer, remote_writes), method, path, remote_writes);
}

/* remote_writes.global_ignored: requests refused that the retired
 * aimee.api.remote_writes would formerly have allowed. Request threads run
 * concurrently, so it carries its own lock rather than racing on a plain
 * increment or borrowing an unrelated one. */
static uint64_t g_remote_writes_global_ignored = 0;
static pthread_mutex_t g_global_ignored_lock = PTHREAD_MUTEX_INITIALIZER;

void server_http_note_global_ignored(void)
{
   pthread_mutex_lock(&g_global_ignored_lock);
   g_remote_writes_global_ignored++;
   pthread_mutex_unlock(&g_global_ignored_lock);
}

uint64_t server_http_global_ignored_count(void)
{
   pthread_mutex_lock(&g_global_ignored_lock);
   uint64_t value = g_remote_writes_global_ignored;
   pthread_mutex_unlock(&g_global_ignored_lock);
   return value;
}

int server_http_retired_global_would_allow(int fd, int is_tcp, const char *bearer,
                                           int remote_writes, int mtls_mode, int mtls_authenticated,
                                           const char *method, const char *path)
{
   (void)fd;
   (void)mtls_mode;
   (void)mtls_authenticated;
   if (remote_writes == SERVER_REMOTE_WRITES_OFF)
      return 0;
   /* Reconstruct the retired switch itself, not the request's current mTLS-
    * narrowed capability set. The old global authorizer derived both its caps
    * and its route tier from remote_writes; using current effective caps makes
    * the observability counter disappear in optional-mTLS bearer fallback. */
   return server_http_route_allowed(is_tcp, bearer, method, path, remote_writes);
}

int server_http_resolve_write_tier(int is_tcp, const char *buf, const char *method,
                                   const char *path, const char *request_id,
                                   server_identity_token_claims_t *claims, int *identity_present)
{
   if (claims)
      memset(claims, 0, sizeof(*claims));
   if (identity_present)
      *identity_present = 0;
   if (!claims || !identity_present)
      return SERVER_REMOTE_WRITES_OFF;
   /* UDS is structurally exempt: the local operator is OS-attested and keeps
    * full capability (§7), so no token is looked for at all. */
   if (!is_tcp)
      return SERVER_REMOTE_WRITES_OFF;

   char identity[KB_IDENTITY_TOKEN_WIRE_MAX + 1] = "";
   char auth_value[KB_IDENTITY_TOKEN_WIRE_MAX + 1] = "";
   size_t identity_len = 0;
   if (http_header(buf, "Authorization", auth_value, sizeof(auth_value)))
   {
      const char *bearer = aimee_core_bearer_token(auth_value);
      const char *credential = bearer ? bearer : auth_value;
      while (*credential == ' ')
         credential++;
      /* Only a compact JWS can be an identity token. The legacy shared bearer is
       * an opaque string, so this leaves it alone instead of reporting every
       * legacy read as a malformed token. */
      if (strchr(credential, '.'))
      {
         identity_len = strnlen(credential, sizeof(identity) - 1);
         memcpy(identity, credential, identity_len);
         identity[identity_len] = '\0';
      }
   }
   server_write_tier_outcome_t outcome = SERVER_WRITE_TIER_ABSENT;
   int tier = server_write_tier_verify_for_request(identity_len ? identity : NULL, identity_len,
                                                   (int64_t)time(NULL), &outcome, claims);
   *identity_present = outcome == SERVER_WRITE_TIER_OK;
   if (outcome != SERVER_WRITE_TIER_OK && outcome != SERVER_WRITE_TIER_ABSENT)
      LOG_INFO("server.http", "%s %s write tier denied (%s) req_id=%s", method, path,
               server_write_tier_outcome_str(outcome), request_id);
   OPENSSL_cleanse(identity, sizeof(identity));
   OPENSSL_cleanse(auth_value, sizeof(auth_value));
   return tier;
}
