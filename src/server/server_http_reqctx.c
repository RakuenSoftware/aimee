/* server_http_reqctx.c: per-request context population for the /v1 HTTP front-end
 * (#3). Extracted from server_http.c (which sits at the line-count limit). Reads
 * the connection socket + request headers into the thread-local request context
 * the ingress handlers, the audit writer, and §4 dedup consume. */
/* _GNU_SOURCE: struct ucred / SO_PEERCRED peer-credential capture is a GNU
 * extension; declare it before any include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_http.h"
#include "aimee/core/connection/auth.h"
#include "kb_identity_token.h"
#include "request_context.h"
#include "config.h"
#include "runtime_secret.h"
#include <netinet/in.h> /* INADDR_ANY / INADDR_LOOPBACK */
#include <openssl/crypto.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* Pure bind-address decision for a TCP /v1 listener (see server_http.h). A
 * plaintext listener (allow_external == 0) is pinned to loopback even when an
 * external bind is requested, so credentials never face the network in cleartext;
 * only the TLS listener may bind 0.0.0.0. */
uint32_t server_http_resolve_bind_addr(int want_external, int allow_external)
{
   return (want_external && allow_external) ? INADDR_ANY : INADDR_LOOPBACK;
}

/* Constant-time string compare (avoid leaking the proxy secret via timing). */
static int reqctx_ct_equal(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   size_t la = strlen(a), lb = strlen(b);
   unsigned char diff = (unsigned char)(la ^ lb);
   size_t n = la > lb ? la : lb;
   for (size_t i = 0; i < n; i++)
      diff |= (unsigned char)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
   return diff == 0;
}

/* Capture the Unix-domain peer's uid via SO_PEERCRED; returns -1 on TCP or when
 * the platform/socket cannot report it. */
static long reqctx_peer_uid(int fd, int is_tcp)
{
   if (is_tcp)
      return -1;
#ifdef SO_PEERCRED
   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
      return (long)cred.uid;
#endif
   return -1;
}

int server_http_host_subject_for_uid(long uid, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (uid < 0 || !out || cap == 0)
      return -1;
   struct passwd pwd;
   struct passwd *resolved = NULL;
   char scratch[16384];
   if (getpwuid_r((uid_t)uid, &pwd, scratch, sizeof(scratch), &resolved) != 0 || !resolved ||
       !resolved->pw_name || !resolved->pw_name[0])
   {
      out[0] = '\0';
      return -1;
   }
   int n = snprintf(out, cap, "%s", resolved->pw_name);
   if (n <= 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

int server_http_apply_caller_context(int is_tcp, const char *request,
                                     const char *first_user_principal, int identity_present,
                                     const char *identity_subject)
{
   const request_context_t *ctx = request_context_get();
   if (!is_tcp && (!ctx || ctx->peer_uid < 0 || !request_context_caller_subject()[0]))
      return -1;
   if (first_user_principal && first_user_principal[0])
   {
      request_context_override_principal(first_user_principal);
      request_context_override_caller_subject(!strncmp(first_user_principal, "webuser:", 8)
                                                  ? first_user_principal + 8
                                                  : first_user_principal);
   }
   if (!identity_present || !identity_subject || !identity_subject[0])
      return 0;
   if (strncmp(identity_subject, "oidc:", 5) != 0)
   {
      request_context_override_caller_subject(identity_subject);
      return 0;
   }

   /* Preserve the original KB-signed identity token. The server verifies it for
    * its own gates; the KB verifies it again before accepting an OIDC caller. */
   char authorization[KB_IDENTITY_TOKEN_WIRE_MAX + 1] = "";
   if (http_header(request, "Authorization", authorization, sizeof(authorization)))
   {
      const char *jwt = aimee_core_bearer_token(authorization);
      if (jwt)
         request_context_override_caller_authorization(jwt);
   }
   OPENSSL_cleanse(authorization, sizeof(authorization));
   return 0;
}

static void reqctx_caller_from_principal(request_context_t *ctx, const char *principal)
{
   if (!ctx || !principal || !principal[0])
      return;
   static const char webuser_prefix[] = "webuser:";
   if (strncmp(principal, webuser_prefix, sizeof(webuser_prefix) - 1) == 0)
      principal += sizeof(webuser_prefix) - 1;
   /* uid:<n> is a vault/audit principal, not a KB subject. The UDS path resolves
    * it through getpwuid_r below instead of forwarding the numeric label. */
   if (strncmp(principal, "uid:", 4) != 0 && principal[0])
      snprintf(ctx->caller_subject, sizeof(ctx->caller_subject), "%s", principal);
}

/* Populate the thread-local request context (#3) from the socket and headers so
 * the buffered ingress handlers, the audit writer, and §4 dedup can read a
 * request id, idempotency key, principal, and transport without threading them
 * through every signature.
 *
 * Trust model: the principal is, by default, derived from the KERNEL-VERIFIED
 * Unix-domain peer uid (uid:N) — the server's own attribution, which a client
 * cannot forge. A client/proxy-supplied X-Aimee-Principal / X-Aimee-Source /
 * X-Aimee-Session-Key is honoured ONLY when the request presents
 * X-Aimee-Proxy-Authorization equal to the configured ingress_trusted_proxy_secret.
 * Being on the local socket is NOT sufficient — a same-host, same-uid non-proxy
 * client must not be able to spoof another account's attribution. With no secret
 * configured, no client-supplied principal/source/session is ever trusted. The
 * idempotency key is read on any transport (it only ever dedups the caller's own
 * identical requests). */
void server_http_populate_request_context(int fd, int is_tcp, const char *buf,
                                          const char *request_id, const char *method,
                                          const char *path, uint32_t caps)
{
   request_context_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.method, sizeof(ctx.method), "%s", method ? method : "");
   snprintf(ctx.path, sizeof(ctx.path), "%s", path ? path : "");
   snprintf(ctx.request_id, sizeof(ctx.request_id), "%s", request_id ? request_id : "");
   ctx.transport = is_tcp ? REQ_TRANSPORT_TCP : REQ_TRANSPORT_UDS;
   ctx.peer_uid = reqctx_peer_uid(fd, is_tcp);
   ctx.capabilities = caps;
   ctx.trusted = 0;

   http_header(buf, "Idempotency-Key", ctx.idempotency_key, sizeof(ctx.idempotency_key));

   /* X-Aimee-Compress: 0 opts this request out of ingress envelope compression
    * (ingress-compression P1b §1.4/B1). Not identity, so it needs no trusted
    * proxy — any caller may disable it for its own turn; it never forces it on. */
   char compress_hdr[16] = "";
   if (http_header(buf, "X-Aimee-Compress", compress_hdr, sizeof(compress_hdr)) &&
       strcmp(compress_hdr, "0") == 0)
      ctx.compress_disabled = 1;

   /* Server-derived principal from the kernel-verified UDS peer uid. */
   if (ctx.peer_uid >= 0)
   {
      snprintf(ctx.principal, sizeof(ctx.principal), "uid:%ld", ctx.peer_uid);
      (void)server_http_host_subject_for_uid(ctx.peer_uid, ctx.caller_subject,
                                             sizeof(ctx.caller_subject));
   }

   /* The root-owned webchat proxy is kernel-attested over the Unix socket. Root
    * already controls the host/container, so a second shared secret adds only a
    * plaintext credential to manage. TCP proxies still require a vaulted secret. */
   if (!is_tcp && ctx.peer_uid == 0)
      ctx.trusted = 1;

   /* A proxy is trusted to stamp identity only by presenting the Vault-backed
    * runtime secret. Config snapshots never carry credentials. */
   char vault_secret[160] = "";
   const char *secret = "";
   if (runtime_secret_get("AIMEE_INGRESS_PROXY_SECRET", vault_secret, sizeof(vault_secret)))
      secret = vault_secret;
   if (!ctx.trusted && secret[0])
   {
      char proxy_auth[160] = "";
      if (http_header(buf, "X-Aimee-Proxy-Authorization", proxy_auth, sizeof(proxy_auth)) &&
          reqctx_ct_equal(proxy_auth, secret))
         ctx.trusted = 1;
   }
   runtime_secret_wipe(vault_secret, sizeof(vault_secret));

   /* The principal, source, AND session key are attribution identity that aimee
    * trusts onto the audit row, so they are honoured ONLY from a trusted proxy
    * (the secret matched above). A plain authorized TCP client or a same-uid UDS
    * client is NOT a trusted proxy and cannot choose its audit principal/source/
    * session. */
   if (ctx.trusted)
   {
      char principal[128] = "";
      char source[64] = "";
      if (http_header(buf, "X-Aimee-Principal", principal, sizeof(principal)) && principal[0])
      {
         snprintf(ctx.principal, sizeof(ctx.principal), "%s", principal);
         reqctx_caller_from_principal(&ctx, principal);
      }
      if (http_header(buf, "X-Aimee-Source", source, sizeof(source)) && source[0])
         snprintf(ctx.source, sizeof(ctx.source), "%s", source);
      http_header(buf, "X-Aimee-Session-Key", ctx.session_key, sizeof(ctx.session_key));
   }

   request_context_set(&ctx);
}
