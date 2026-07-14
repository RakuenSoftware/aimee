/* windows/agent_bridge.c: Windows provider layer — HTTP client (WinHTTP) and SSH tunnel lifecycle
 */
#include "aimee.h"
#include "agent_exec.h"
#include <string.h>
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
   (void)extra_headers; /* Phase 2: add WinHTTP extra headers support */
   *response_buf = NULL;

   /* Parse URL into components */
   wchar_t wurl[2048];
   MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);

   URL_COMPONENTS uc;
   memset(&uc, 0, sizeof(uc));
   uc.dwStructSize = sizeof(uc);
   wchar_t host[256], path[1024];
   uc.lpszHostName = host;
   uc.dwHostNameLength = 256;
   uc.lpszUrlPath = path;
   uc.dwUrlPathLength = 1024;

   if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
      return -1;

   HINTERNET session = WinHttpOpen(L"aimee/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
   if (!session)
      return -1;

   /* Set timeouts */
   WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

   BOOL use_ssl = (uc.nScheme == INTERNET_SCHEME_HTTPS);
   HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
   if (!conn)
   {
      WinHttpCloseHandle(session);
      return -1;
   }

   DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
   HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
   if (!req)
   {
      WinHttpCloseHandle(conn);
      WinHttpCloseHandle(session);
      return -1;
   }

   /* Add headers. Accept a full header line or a bare media type: WinHttp needs a
    * complete "Field: value" line, so prefix "Content-Type: " when the caller
    * passed only the media type (mirrors the POSIX path). */
   const char *ct_in =
       content_type && content_type[0] ? content_type : "Content-Type: application/json";
   char ct[256];
   snprintf(ct, sizeof ct, "%s%s", strchr(ct_in, ':') ? "" : "Content-Type: ", ct_in);
   wchar_t wct[512];
   MultiByteToWideChar(CP_UTF8, 0, ct, -1, wct, 512);
   WinHttpAddRequestHeaders(req, wct, -1, WINHTTP_ADDREQ_FLAG_ADD);

   if (auth_header && auth_header[0])
   {
      wchar_t wauth[1024];
      MultiByteToWideChar(CP_UTF8, 0, auth_header, -1, wauth, 1024);
      WinHttpAddRequestHeaders(req, wauth, -1, WINHTTP_ADDREQ_FLAG_ADD);
   }

   /* Send */
   DWORD body_len = (DWORD)strlen(body);
   if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)body, body_len, body_len,
                           0) ||
       !WinHttpReceiveResponse(req, NULL))
   {
      WinHttpCloseHandle(req);
      WinHttpCloseHandle(conn);
      WinHttpCloseHandle(session);
      return -1;
   }

   /* Read status */
   DWORD status_code = 0;
   DWORD sz = sizeof(status_code);
   WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz, WINHTTP_NO_HEADER_INDEX);

   /* Read body */
   char *result = NULL;
   size_t result_len = 0;
   DWORD bytes_available, bytes_read;

   while (WinHttpQueryDataAvailable(req, &bytes_available) && bytes_available > 0)
   {
      char *tmp = realloc(result, result_len + bytes_available + 1);
      if (!tmp)
      {
         free(result);
         result = NULL;
         break;
      }
      result = tmp;
      WinHttpReadData(req, result + result_len, bytes_available, &bytes_read);
      result_len += bytes_read;
      result[result_len] = '\0';
   }

   WinHttpCloseHandle(req);
   WinHttpCloseHandle(conn);
   WinHttpCloseHandle(session);

   *response_buf = result;
   return (int)status_code;
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
