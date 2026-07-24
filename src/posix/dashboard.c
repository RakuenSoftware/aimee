/* dashboard.c: POSIX HTTP dashboard server (Linux/macOS) */
#include "aimee.h"
#include "dashboard.h"
#include "kb_client.h"
#include "platform_path.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DASHBOARD_DEFAULT_PORT 9200
#define DASHBOARD_MAX_REQUEST  8192

/* CORS state defined in dashboard.c, accessible here for cors_seed and cors_check */
extern char cors_origins[CORS_MAX_ORIGINS][CORS_ORIGIN_LEN];
extern int cors_origin_count;

/* cors_load() defined in dashboard.c */
void cors_load(void);

static const char *dashboard_html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>aimee dashboard</title>"
    "<style>"
    "body{font-family:system-ui;margin:0;padding:20px;background:#111;color:#eee}"
    "h1{margin:0 0 20px;font-size:24px;color:#8cf}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}"
    ".card{background:#1a1a2e;border-radius:8px;padding:16px;border:1px solid #333}"
    ".card h2{margin:0 0 12px;font-size:16px;color:#adf}"
    "table{width:100%;border-collapse:collapse;font-size:13px}"
    "th{text-align:left;padding:6px 8px;border-bottom:1px solid #444;color:#888}"
    "td{padding:6px 8px;border-bottom:1px solid #222}"
    ".ok{color:#4f4}.err{color:#f44}.warn{color:#fa0}"
    ".badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px}"
    ".badge.success{background:#143;color:#4f4}"
    ".badge.error{background:#411;color:#f44}"
    ".badge.running{background:#431;color:#fa0}"
    ".badge.warn{background:#431;color:#fa0}"
    ".num{text-align:right;font-variant-numeric:tabular-nums}"
    "#refresh{position:fixed;top:16px;right:16px;padding:6px 14px;"
    "background:#234;color:#8cf;border:1px solid #456;border-radius:4px;cursor:pointer}"
    "</style></head><body>"
    "<h1>aimee dashboard</h1>"
    "<button id='refresh' onclick='load()'>Refresh</button>"
    "<div class='grid'>"
    "<div class='card'><h2>Health</h2><div id='health'></div></div>"
    "<div class='card'><h2>Recent Delegations</h2><div id='delegations'></div></div>"
    "<div class='card'><h2>Metrics by Role</h2><div id='metrics'></div></div>"
    "<div class='card'><h2>Recent Traces</h2><div id='traces'></div></div>"
    "<div class='card'><h2>Memory Stats</h2><div id='memory'></div></div>"
    "<div class='card'><h2>Execution Plans</h2><div id='plans'></div></div>"
    "</div>"
    "<script>"
    "function e(s){if(s==null)return'';return String(s).replace(/&/g,'&amp;')"
    ".replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;')}"
    "async function load(){"
    "let h=await(await fetch('/api/doctor')).json();"
    "let d=await(await fetch('/api/delegations')).json();"
    "let m=await(await fetch('/api/metrics')).json();"
    "let t=await(await fetch('/api/traces')).json();"
    "let s=await(await fetch('/api/memory-stats')).json();"
    "let p=await(await fetch('/api/plans')).json();"
    "var hcls=h.errors>0?'error':h.warnings>0?'warn':'success';"
    "document.getElementById('health').innerHTML="
    "'<div style=\"margin-bottom:8px\"><span class=\"badge '+hcls+'\">'"
    "+(h.errors>0?h.errors+' error(s)':h.warnings>0?h.warnings+' warning(s)':'All OK')"
    "+'</span></div><table><tr><th>Check</th><th>Status</th><th>Detail</th></tr>'"
    "+(h.checks||[]).map(function(r){"
    "var cls=r.status==='OK'?'success':r.status==='ERROR'?'error':'warn';"
    "return '<tr><td>'+e(r.name)+'</td><td><span class=\"badge '+cls+'\">'"
    "+e(r.status)+'</span></td><td>'+e(r.message)+(r.remediation?"
    "' &mdash; <em style=\"color:#888\">'+e(r.remediation)+'</em>':'')"
    "+'</td></tr>';}).join('')+'</table>';"
    "document.getElementById('delegations').innerHTML='<table><tr><th>Agent</th><th>Role</th>"
    "<th>Status</th><th>Turns</th><th>Tools</th><th>Confidence</th><th>Latency</th></tr>'"
    "+d.map(r=>'<tr><td>'+e(r.agent)+'</td><td>'+e(r.role)+'</td><td><span class=\"badge '+"
    "(r.success?'success':'error')+'\">'"
    "+(r.success?'OK':'ERR')+'</span></td><td class=\"num\">'+r.turns+'</td>"
    "<td class=\"num\">'+r.tool_calls+'</td><td class=\"num\">'+(r.confidence>=0?r.confidence:'--')"
    "+'</td><td class=\"num\">'+r.latency_ms+'ms</td></tr>').join('')+'</table>';"
    "document.getElementById('metrics').innerHTML='<table><tr><th>Role</th><th>Total</th>"
    "<th>Success</th><th>Avg Lat</th><th>Tokens</th></tr>'"
    "+m.map(r=>'<tr><td>'+e(r.role)+'</td><td class=\"num\">'+r.total+'</td>"
    "<td class=\"num\">'+r.successes+'</td><td class=\"num\">'+r.avg_latency_ms+'ms</td>"
    "<td class=\"num\">'+r.tokens+'</td></tr>').join('')+'</table>';"
    "document.getElementById('traces').innerHTML='<table><tr><th>Turn</th><th>Tool</th>"
    "<th>Direction</th></tr>'"
    "+t.map(r=>'<tr><td class=\"num\">'+r.turn+'</td><td>'"
    "+e(r.tool_name||'--')+'</td><td>'+e(r.direction)+'</td></tr>').join('')+'</table>';"
    "document.getElementById('memory').innerHTML='<table><tr><th>Tier</th><th>Kind</th>"
    "<th>Count</th></tr>'"
    "+s.map(r=>'<tr><td>'+e(r.tier)+'</td><td>'+e(r.kind)+'</td>"
    "<td class=\"num\">'+r.count+'</td></tr>').join('')+'</table>';"
    "document.getElementById('plans').innerHTML='<table><tr><th>ID</th><th>Agent</th>"
    "<th>Status</th><th>Steps</th><th>Task</th></tr>'"
    "+p.map(r=>'<tr><td class=\"num\">'+r.id+'</td><td>'+e(r.agent)+'</td>"
    "<td><span class=\"badge "
    "'+(r.status==\"done\"?'success':r.status==\"failed\"?'error':'running')"
    "+'\">'+e(r.status)+'</span></td><td class=\"num\">'+r.done_steps+'/'+r.total_steps+'</td>"
    "<td>'+e(r.task.substring(0,60))+'</td></tr>').join('')+'</table>';"
    "}"
    "load();setInterval(load,15000);"
    "</script></body></html>";

/* Auto-detect local network IPs and add them as allowed origins.
 * Adds http://<ip>:<common-ports> for each non-loopback interface. */
static void cors_seed_local_networks(void)
{
   struct ifaddrs *ifap = NULL;
   if (getifaddrs(&ifap) != 0)
      return;

   for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next)
   {
      if (!ifa->ifa_addr)
         continue;
      if (!(ifa->ifa_flags & IFF_UP))
         continue;
      if (ifa->ifa_flags & IFF_LOOPBACK)
         continue;

      char ip[INET6_ADDRSTRLEN] = {0};
      if (ifa->ifa_addr->sa_family == AF_INET)
      {
         struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
         inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
      }
      else if (ifa->ifa_addr->sa_family == AF_INET6)
      {
         struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
         /* Skip link-local fe80:: addresses */
         if (sa6->sin6_addr.s6_addr[0] == 0xfe && sa6->sin6_addr.s6_addr[1] == 0x80)
            continue;
         inet_ntop(AF_INET6, &sa6->sin6_addr, ip, sizeof(ip));
      }
      else
      {
         continue;
      }

      if (!ip[0])
         continue;

      /* Add common port variants as allowed origins */
      static const int ports[] = {80, 443, 3000, 5173, 8080, 9200, 0};
      for (int pi = 0; ports[pi] && cors_origin_count < CORS_MAX_ORIGINS; pi++)
      {
         char origin[CORS_ORIGIN_LEN];
         if (ports[pi] == 80)
            snprintf(origin, sizeof(origin), "http://%s", ip);
         else if (ports[pi] == 443)
            snprintf(origin, sizeof(origin), "https://%s", ip);
         else
            snprintf(origin, sizeof(origin), "http://%s:%d", ip, ports[pi]);

         /* Check for duplicate */
         int dup = 0;
         for (int i = 0; i < cors_origin_count; i++)
         {
            if (strcmp(cors_origins[i], origin) == 0)
            {
               dup = 1;
               break;
            }
         }
         if (!dup && cors_origin_count < CORS_MAX_ORIGINS)
         {
            snprintf(cors_origins[cors_origin_count++], CORS_ORIGIN_LEN, "%s", origin);
         }
      }
   }

   freeifaddrs(ifap);
}

/* Check if a request Origin is in the allowed CORS list */
static const char *cors_check_origin(const char *request_buf)
{
   if (cors_origin_count == 0)
      return NULL;

   /* Find "Origin: " header in request */
   const char *hdr = strstr(request_buf, "Origin: ");
   if (!hdr)
      hdr = strstr(request_buf, "origin: ");
   if (!hdr)
      return NULL;

   hdr += 8; /* skip "Origin: " */
   char origin[CORS_ORIGIN_LEN];
   int oi = 0;
   while (*hdr && *hdr != '\r' && *hdr != '\n' && oi < CORS_ORIGIN_LEN - 1)
      origin[oi++] = *hdr++;
   origin[oi] = '\0';

   for (int i = 0; i < cors_origin_count; i++)
   {
      if (strcmp(cors_origins[i], origin) == 0)
         return cors_origins[i];
   }
   return NULL;
}

/* --- PAM-based HTTP Basic Auth (optional, requires -DWITH_PAM and -lpam) --- */

#if defined(__linux__) && defined(WITH_PAM)
#include <security/pam_appl.h>

static int pam_conversation(int num_msg, const struct pam_message **msg, struct pam_response **resp,
                            void *appdata_ptr)
{
   const char *password = (const char *)appdata_ptr;
   struct pam_response *reply = calloc((size_t)num_msg, sizeof(struct pam_response));
   if (!reply)
      return PAM_BUF_ERR;
   for (int i = 0; i < num_msg; i++)
   {
      if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
         reply[i].resp = strdup(password);
   }
   *resp = reply;
   return PAM_SUCCESS;
}

int pam_check_credentials(const char *user, const char *password)
{
   struct pam_conv conv = {pam_conversation, (void *)password};
   pam_handle_t *pamh = NULL;
   int rc = pam_start("aimee", user, &conv, &pamh);
   if (rc != PAM_SUCCESS)
      return 0;
   rc = pam_authenticate(pamh, PAM_SILENT);
   int ok = (rc == PAM_SUCCESS);
   if (ok)
      rc = pam_acct_mgmt(pamh, PAM_SILENT);
   ok = ok && (rc == PAM_SUCCESS);
   pam_end(pamh, rc);
   return ok;
}
#else
int pam_check_credentials(const char *user, const char *password)
{
   (void)user;
   (void)password;
   return 0; /* PAM not available -- reject all credentials */
}
#endif

/* Base64 decode (minimal, for Authorization header) */
int base64_decode(const char *in, char *out, size_t out_len)
{
   static const unsigned char d[256] = {
       ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,
       ['H'] = 7,  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13,
       ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20,
       ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27,
       ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34,
       ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
       ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48,
       ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
       ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62,
       ['/'] = 63};
   size_t len = strlen(in);
   /* Reject malformed input that isn't a multiple of 4 */
   if (len == 0 || len % 4 != 0)
   {
      out[0] = '\0';
      return 0;
   }
   size_t oi = 0;
   for (size_t i = 0; i + 3 < len && oi < out_len - 1; i += 4)
   {
      unsigned int n = (d[(unsigned char)in[i]] << 18) | (d[(unsigned char)in[i + 1]] << 12) |
                       (d[(unsigned char)in[i + 2]] << 6) | d[(unsigned char)in[i + 3]];
      out[oi++] = (char)((n >> 16) & 0xFF);
      if (in[i + 2] != '=' && oi < out_len - 1)
         out[oi++] = (char)((n >> 8) & 0xFF);
      if (in[i + 3] != '=' && oi < out_len - 1)
         out[oi++] = (char)(n & 0xFF);
   }
   out[oi] = '\0';
   return (int)oi;
}

static int check_basic_auth(const char *request_buf, int fd)
{
#ifndef __linux__
   (void)request_buf;
   (void)fd;
   return 1; /* PAM not available, allow */
#else
   const char *auth = strstr(request_buf, "Authorization: Basic ");
   if (!auth)
      auth = strstr(request_buf, "authorization: Basic ");
   if (!auth)
   {
      const char *resp = "HTTP/1.1 401 Unauthorized\r\n"
                         "WWW-Authenticate: Basic realm=\"aimee\"\r\n"
                         "Content-Length: 12\r\n"
                         "Connection: close\r\n\r\n"
                         "Unauthorized";
      (void)write(fd, resp, strlen(resp));
      return 0;
   }

   auth += 21; /* skip "Authorization: Basic " */
   char encoded[256] = {0};
   int ei = 0;
   while (*auth && *auth != '\r' && *auth != '\n' && ei < 255)
      encoded[ei++] = *auth++;
   encoded[ei] = '\0';

   char decoded[256];
   base64_decode(encoded, decoded, sizeof(decoded));

   char *colon = strchr(decoded, ':');
   if (!colon)
   {
      const char *resp = "HTTP/1.1 401 Unauthorized\r\n"
                         "WWW-Authenticate: Basic realm=\"aimee\"\r\n"
                         "Content-Length: 12\r\n"
                         "Connection: close\r\n\r\n"
                         "Unauthorized";
      (void)write(fd, resp, strlen(resp));
      return 0;
   }
   *colon = '\0';
   const char *user = decoded;
   const char *pass = colon + 1;

   if (!pam_check_credentials(user, pass))
   {
      const char *resp = "HTTP/1.1 401 Unauthorized\r\n"
                         "WWW-Authenticate: Basic realm=\"aimee\"\r\n"
                         "Content-Length: 12\r\n"
                         "Connection: close\r\n\r\n"
                         "Unauthorized";
      (void)write(fd, resp, strlen(resp));
      return 0;
   }
   return 1;
#endif
}

/* --- HTTP server --- */

static void send_response_cors(int fd, int status, const char *content_type, const char *body,
                               const char *allowed_origin)
{
   char header[768];
   int body_len = body ? (int)strlen(body) : 0;
   int hlen;
   if (allowed_origin)
   {
      hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "Access-Control-Allow-Origin: %s\r\n"
                      "Vary: Origin\r\n"
                      "Connection: close\r\n\r\n",
                      status, status == 200 ? "OK" : "Not Found", content_type, body_len,
                      allowed_origin);
   }
   else
   {
      hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n\r\n",
                      status, status == 200 ? "OK" : "Not Found", content_type, body_len);
   }
   (void)write(fd, header, (size_t)hlen);
   if (body && body_len > 0)
      (void)write(fd, body, (size_t)body_len);
}

static void send_response(int fd, int status, const char *content_type, const char *body)
{
   send_response_cors(fd, status, content_type, body, NULL);
}

static void handle_request(int fd)
{
   char buf[DASHBOARD_MAX_REQUEST];
   ssize_t total = 0;
   while (total < (ssize_t)sizeof(buf) - 1)
   {
      ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
      if (n <= 0)
         break;
      total += n;
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n"))
         break;
   }
   if (total <= 0)
      return;

   /* Parse request path */
   char method[16] = {0}, path[256] = {0};
   sscanf(buf, "%15s %255s", method, path);
   /* Strip query string for path matching */
   char *qmark = strchr(path, '?');
   if (qmark)
      *qmark = '\0';

   /* Reject non-GET methods */
   if (strcmp(method, "GET") != 0)
   {
      send_response(fd, 405, "text/plain", "Method Not Allowed");
      return;
   }

   /* URL-decode path to catch encoded traversal (%2e%2e) */
   {
      char decoded[256];
      size_t di = 0;
      for (size_t si = 0; path[si] && di < sizeof(decoded) - 1; si++)
      {
         if (path[si] == '%' && path[si + 1] && path[si + 2])
         {
            char hex[3] = {path[si + 1], path[si + 2], 0};
            char ch = (char)strtol(hex, NULL, 16);
            if (ch)
            {
               decoded[di++] = ch;
               si += 2;
               continue;
            }
         }
         decoded[di++] = path[si];
      }
      decoded[di] = '\0';
      snprintf(path, sizeof(path), "%s", decoded);
   }

   /* Reject paths with traversal */
   if (strstr(path, ".."))
   {
      send_response(fd, 400, "text/plain", "Bad Request");
      return;
   }

   /* Check CORS origin for API requests */
   const char *allowed_origin = cors_check_origin(buf);

   /* Require Basic Auth for all dashboard access */
   if (!check_basic_auth(buf, fd))
      return;

   if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
   {
      send_response_cors(fd, 200, "text/html", dashboard_html, allowed_origin);
   }
   else if (strcmp(path, "/api/delegations") == 0)
   {
      char *json = api_delegations();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/metrics") == 0)
   {
      char *json = api_metrics();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/vector-status") == 0)
   {
      char *json = api_vector_status();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/traces") == 0)
   {
      char *json = api_traces();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/plans") == 0)
   {
      char *json = api_plans();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/logs") == 0)
   {
      char *json = kb_client_dashboard_logs_json();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/dashboard/maintenance") == 0)
   {
      char *json = api_dashboard_maintenance();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/dashboard/identity") == 0)
   {
      char *json = api_dashboard_identity();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/bench") == 0)
   {
      char *json = api_bench_results();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/doctor") == 0)
   {
      char *json = api_doctor();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/dashboard/payload-rewrite") == 0)
   {
      char *json = api_payload_rewrite_status();
      send_response_cors(fd, 200, "application/json", json, allowed_origin);
      free(json);
   }
   else if (strcmp(path, "/api/memory-stats") == 0 ||
            strcmp(path, "/api/dashboard/reminders") == 0 ||
            strcmp(path, "/api/dashboard/recall") == 0 ||
            strcmp(path, "/api/dashboard/directives") == 0 ||
            strcmp(path, "/api/dashboard/onboard") == 0)
   {
      char *json = NULL;
      if (strcmp(path, "/api/memory-stats") == 0)
         json = kb_client_dashboard_memory_stats_json();
      else if (strcmp(path, "/api/dashboard/reminders") == 0)
         json = kb_client_dashboard_reminders_json();
      else if (strcmp(path, "/api/dashboard/recall") == 0)
         json = kb_client_dashboard_recall_json();
      else if (strcmp(path, "/api/dashboard/directives") == 0)
         json = kb_client_dashboard_directives_json();
      else if (strcmp(path, "/api/dashboard/onboard") == 0)
         json = api_dashboard_onboard();

      send_response_cors(fd, 200, "application/json", json ? json : "[]", allowed_origin);
      free(json);
   }
   else
   {
      send_response(fd, 404, "text/plain", "Not Found");
   }
}

void dashboard_serve(int port)
{
   if (port <= 0)
      port = DASHBOARD_DEFAULT_PORT;

   /* Load explicitly configured CORS origins, then auto-detect local networks */
   cors_load();
   cors_seed_local_networks();

   /* Always allow localhost origins */
   dashboard_cors_add("http://127.0.0.1");
   dashboard_cors_add("http://localhost");
   char local_origin[CORS_ORIGIN_LEN];
   snprintf(local_origin, sizeof(local_origin), "http://127.0.0.1:%d", port);
   dashboard_cors_add(local_origin);
   snprintf(local_origin, sizeof(local_origin), "http://localhost:%d", port);
   dashboard_cors_add(local_origin);

   int srv = socket(AF_INET, SOCK_STREAM, 0);
   if (srv < 0)
      fatal("socket failed");

   int opt = 1;
   setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = htons((uint16_t)port);

   if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      fatal("bind failed on port %d", port);

   if (listen(srv, 8) < 0)
      fatal("listen failed");

   printf("aimee dashboard: http://127.0.0.1:%d\n", port);
   fflush(stdout);

   while (1)
   {
      int client = accept(srv, NULL, NULL);
      if (client < 0)
         continue;
      handle_request(client);
      close(client);
   }
}
