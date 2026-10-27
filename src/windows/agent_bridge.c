/* windows/agent_bridge.c: Windows provider layer — HTTP client (WinHTTP) and SSH tunnel lifecycle
 */
#include "aimee.h"
#include "agent_exec.h"
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winhttp.h>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

void agent_http_init(void)
{
   /* WinHTTP does not need global init */
}

void agent_http_cleanup(void)
{
}

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   return agent_http_post_content_type(url, auth_header, "Content-Type: application/json", body,
                                       response_buf, timeout_ms, extra_headers);
}

int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers)
{
   size_t response_len = 0;
   return agent_http_post_content_type_bytes(url, auth_header, content_type, body,
                                             body ? strlen(body) : 0, response_buf, &response_len,
                                             timeout_ms, extra_headers);
}

/* Reject failed conversions before passing any wchar buffer to WinHTTP. */
static wchar_t *http_utf8_wide(const char *text)
{
   if (!text)
      return NULL;
   int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
   if (length <= 0 || (size_t)length > SIZE_MAX / sizeof(wchar_t))
      return NULL;
   wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
   if (!wide)
      return NULL;
   if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) != length)
   {
      free(wide);
      return NULL;
   }
   return wide;
}

int agent_http_post_content_type_bytes(const char *url, const char *auth_header,
                                       const char *content_type, const void *body, size_t body_len,
                                       char **response_buf, size_t *response_len, int timeout_ms,
                                       const char *extra_headers)
{
   (void)extra_headers; /* Phase 2: add WinHTTP extra headers support */
   if (response_buf)
      *response_buf = NULL;
   if (response_len)
      *response_len = 0;
   if (!response_buf || !response_len || (!body && body_len) || body_len > MAXDWORD)
      return -1;

   int result_status = -1;
   HINTERNET session = NULL, conn = NULL, req = NULL;
   char *ct = NULL, *result = NULL;
   wchar_t *wurl = http_utf8_wide(url), *host = NULL, *path = NULL;
   wchar_t *wct = NULL, *wauth = NULL;
   size_t result_len = 0;
   if (!wurl)
      goto done;
   /* URL component buffers are bounded by the actual converted URL, not fixed
    * host/path scratch buffers that truncate or reject otherwise valid URLs. */
   size_t url_length = wcslen(wurl) + 1;
   host = calloc(url_length, sizeof(wchar_t));
   path = calloc(url_length, sizeof(wchar_t));
   if (!host || !path)
      goto done;
   URL_COMPONENTS uc;
   memset(&uc, 0, sizeof(uc));
   uc.dwStructSize = sizeof(uc);
   uc.lpszHostName = host;
   uc.dwHostNameLength = (DWORD)url_length;
   uc.lpszUrlPath = path;
   uc.dwUrlPathLength = (DWORD)url_length;
   if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
      goto done;

   /* Preserve the full-header and bare-media-type calling conventions. */
   const char *ct_in =
       content_type && content_type[0] ? content_type : "Content-Type: application/json";
   const char *prefix = strchr(ct_in, ':') ? "" : "Content-Type: ";
   size_t ct_length = strlen(ct_in), prefix_length = strlen(prefix);
   if (ct_length > SIZE_MAX - prefix_length - 1)
      goto done;
   ct = malloc(prefix_length + ct_length + 1);
   if (!ct)
      goto done;
   memcpy(ct, prefix, prefix_length);
   memcpy(ct + prefix_length, ct_in, ct_length + 1);
   wct = http_utf8_wide(ct);
   if (!wct)
      goto done;
   if (auth_header && auth_header[0])
   {
      wauth = http_utf8_wide(auth_header);
      if (!wauth)
         goto done;
   }

   session = WinHttpOpen(L"aimee/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
   if (!session)
      goto done;
   WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
   conn = WinHttpConnect(session, host, uc.nPort, 0);
   if (!conn)
      goto done;
   DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
   req = WinHttpOpenRequest(conn, L"POST", path, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
   if (!req || !WinHttpAddRequestHeaders(req, wct, -1, WINHTTP_ADDREQ_FLAG_ADD) ||
       (wauth && !WinHttpAddRequestHeaders(req, wauth, -1, WINHTTP_ADDREQ_FLAG_ADD)))
      goto done;
   if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)body, (DWORD)body_len,
                           (DWORD)body_len, 0) ||
       !WinHttpReceiveResponse(req, NULL))
      goto done;

   DWORD status_code = 0;
   DWORD size = sizeof(status_code);
   if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size,
                            WINHTTP_NO_HEADER_INDEX))
      goto done;
   result = calloc(1, 1);
   if (!result)
      goto done;
   for (;;)
   {
      DWORD available, read;
      if (!WinHttpQueryDataAvailable(req, &available))
         goto done;
      if (!available)
         break;
      if ((size_t)available > SIZE_MAX - result_len - 1)
         goto done;
      char *grown = realloc(result, result_len + available + 1);
      if (!grown)
         goto done;
      result = grown;
      if (!WinHttpReadData(req, result + result_len, available, &read) || !read)
         goto done;
      result_len += read;
      result[result_len] = '\0';
   }
   *response_buf = result;
   *response_len = result_len;
   result = NULL;
   result_status = (int)status_code;
done:
   if (req)
      WinHttpCloseHandle(req);
   if (conn)
      WinHttpCloseHandle(conn);
   if (session)
      WinHttpCloseHandle(session);
   free(wurl);
   free(host);
   free(path);
   free(ct);
   free(wct);
   free(wauth);
   free(result);
   return result_status;
}

int agent_http_get(const char *url, const char *extra_headers, char **response_buf, int timeout_ms)
{
   /* Phase 2: implement WinHTTP GET for Windows support */
   (void)url;
   (void)extra_headers;
   (void)response_buf;
   (void)timeout_ms;
   return -1;
}

int agent_http_get_location(const char *url, const char *extra_headers, char *location,
                            size_t location_cap, char **response_buf, int timeout_ms)
{
   /* Phase 2, with agent_http_get: the forge API path is POSIX-only today. */
   (void)url;
   (void)extra_headers;
   (void)timeout_ms;
   if (location && location_cap)
      location[0] = '\0';
   if (response_buf)
      *response_buf = NULL;
   return -1;
}

int agent_http_put(const char *url, const char *auth_header, const char *body, char **response_buf,
                   int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   (void)extra_headers;
   return -1;
}

int agent_http_patch(const char *url, const char *auth_header, const char *body,
                     char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   (void)extra_headers;
   return -1;
}

int agent_http_get_stream(const char *url, const char *extra_headers, agent_http_stream_cb callback,
                          void *userdata, int timeout_ms)
{
   (void)url;
   (void)extra_headers;
   (void)callback;
   (void)userdata;
   (void)timeout_ms;
   return -1;
}

int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms)
{
   /* Phase 2: implement WinHTTP form POST for Windows support */
   (void)url;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   return -1;
}

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)callback;
   (void)userdata;
   (void)timeout_ms;
   (void)extra_headers;
   return -1;
}

int agent_http_delete(const char *url, const char *auth_header, int timeout_ms)
{
   /* TODO: implement via WinHTTP DELETE request (Phase 2) */
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   return -1;
}

/* ================================================================
 * From: agent_tunnel.c
 * ================================================================ */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "agent_tunnel.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#ifdef AIMEE_POSIX
#include <poll.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef AIMEE_POSIX
#include <sys/wait.h>
#endif
#include <unistd.h>

const char *agent_tunnel_state_str(agent_tunnel_state_t state)
{
   (void)state;
   return "unsupported";
}

void agent_tunnel_mgr_init(agent_tunnel_mgr_t *mgr)
{
   (void)mgr;
}

int agent_tunnel_start_all(agent_tunnel_mgr_t *mgr)
{
   (void)mgr;
   return -1;
}

void agent_tunnel_stop_all(agent_tunnel_mgr_t *mgr)
{
   (void)mgr;
}

void agent_tunnel_mgr_destroy(agent_tunnel_mgr_t *mgr)
{
   (void)mgr;
}

agent_tunnel_t *agent_tunnel_find(agent_tunnel_mgr_t *mgr, const char *name)
{
   (void)mgr;
   (void)name;
   return NULL;
}

int agent_tunnel_resolve_entry(const agent_tunnel_mgr_t *mgr, const agent_network_t *network,
                               const agent_net_host_t *host, char *buf, size_t buf_len)
{
   (void)mgr;
   (void)host;
   if (network && network->ssh_entry[0])
      snprintf(buf, buf_len, "%s", network->ssh_entry);
   else
      buf[0] = '\0';
   return 0;
}

void agent_tunnel_print_status(const agent_tunnel_mgr_t *mgr, int json_output)
{
   (void)mgr;
   (void)json_output;
   printf("Tunnels not supported on this platform.\n");
}
