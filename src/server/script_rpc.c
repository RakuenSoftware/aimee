#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "script_rpc.h"
#include "agent_tools.h"
#include "aimee.h"
#include "cJSON.h"
#include "platform_random.h"
#include "toolset.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCRIPT_RPC_MAX_FRAME  (64 * 1024)
#define SCRIPT_RPC_MAX_RESULT (64 * 1024)

static int script_rpc_write_all(int fd, const void *buf, size_t len)
{
   const char *p = (const char *)buf;
   while (len > 0)
   {
      ssize_t n = write(fd, p, len);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      p += n;
      len -= (size_t)n;
   }
   return 0;
}

static int script_rpc_read_all(int fd, void *buf, size_t len)
{
   char *p = (char *)buf;
   while (len > 0)
   {
      ssize_t n = read(fd, p, len);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      p += n;
      len -= (size_t)n;
   }
   return 0;
}

static int script_rpc_send_json(int fd, cJSON *json)
{
   char *text = cJSON_PrintUnformatted(json);
   if (!text)
      return -1;
   size_t len = strlen(text);
   uint32_t nlen = htonl((uint32_t)len);
   int rc = len <= SCRIPT_RPC_MAX_FRAME && script_rpc_write_all(fd, &nlen, sizeof(nlen)) == 0 &&
                    script_rpc_write_all(fd, text, len) == 0
                ? 0
                : -1;
   free(text);
   return rc;
}

static int script_rpc_allowed_tool(const char *name)
{
   if (!name || !name[0])
      return 0;
   toolset_registry_t registry;
   char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   char err[TOOLSET_ERROR_MAX] = "";
   if (toolset_registry_load_effective(&registry, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "aimee: failed to load script RPC toolsets: %s\n",
              err[0] ? err : "unknown error");
      return 0;
   }
   int n = toolset_resolve(&registry, registry.script_allowed_tools, tools, TOOLSET_MAX_TOOLS, err,
                           sizeof(err));
   if (n < 0)
   {
      fprintf(stderr, "aimee: failed to resolve script RPC toolset '%s': %s\n",
              registry.script_allowed_tools, err[0] ? err : "unknown error");
      return 0;
   }
   for (int i = 0; i < n; i++)
      if (strcmp(name, tools[i]) == 0)
         return 1;
   return 0;
}

static cJSON *script_rpc_error(int id, const char *code, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddNumberToObject(resp, "id", id);
   cJSON_AddBoolToObject(resp, "ok", 0);
   cJSON_AddStringToObject(resp, "error", message ? message : "rpc error");
   cJSON_AddStringToObject(resp, "code", code ? code : "rpc_error");
   return resp;
}

static cJSON *script_rpc_dispatch(script_rpc_server_t *srv, cJSON *req)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *jtool = cJSON_GetObjectItemCaseSensitive(req, "tool");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(req, "args");
   int id = cJSON_IsNumber(jid) ? jid->valueint : 0;
   if (!cJSON_IsNumber(jid) || !cJSON_IsString(jtool) || !cJSON_IsObject(jargs))
      return script_rpc_error(id, "bad_frame", "missing id, tool, or args");

   char *args_json = cJSON_PrintUnformatted(jargs);
   if (!args_json)
      return script_rpc_error(id, "bad_frame", "failed to serialize args");

   if (!script_rpc_allowed_tool(jtool->valuestring))
   {
      srv->policy_denials++;
      free(args_json);
      return script_rpc_error(id, "policy_denied", "tool is not allowed through script RPC");
   }

   /* Reuse the foreground dispatch path so guardrails, role policy, request_input, and approval
    * routing behave the same for nested script calls as for normal tool calls. */
   char *result = dispatch_tool_call_ctx(jtool->valuestring, args_json, 120000);
   srv->tool_calls++;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddNumberToObject(resp, "id", id);
   cJSON_AddBoolToObject(resp, "ok", result && strncmp(result, "error:", 6) != 0);
   if (result)
   {
      if (strlen(result) > SCRIPT_RPC_MAX_RESULT)
         result[SCRIPT_RPC_MAX_RESULT] = '\0';
      cJSON *parsed = cJSON_Parse(result);
      if (parsed)
         cJSON_AddItemToObject(resp, "result", parsed);
      else
         cJSON_AddStringToObject(resp, "result", result);
      if (strncmp(result, "error:", 6) == 0)
         cJSON_AddStringToObject(resp, "error", result);
   }
   else
   {
      cJSON_AddStringToObject(resp, "error", "tool returned no result");
   }
   free(result);
   free(args_json);
   return resp;
}

static void *script_rpc_thread(void *arg)
{
   script_rpc_server_t *srv = (script_rpc_server_t *)arg;
   if (srv->use_fd_pair)
   {
      while (!srv->stop)
      {
         fd_set rfds;
         FD_ZERO(&rfds);
         FD_SET(srv->listen_fd, &rfds);
         struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
         int sel = select(srv->listen_fd + 1, &rfds, NULL, NULL, &tv);
         if (sel < 0 && errno == EINTR)
            continue;
         if (sel <= 0)
            continue;

         uint32_t nlen = 0;
         if (script_rpc_read_all(srv->listen_fd, &nlen, sizeof(nlen)) != 0)
            continue;
         uint32_t len = ntohl(nlen);
         if (len == 0 || len > SCRIPT_RPC_MAX_FRAME)
         {
            cJSON *err = script_rpc_error(0, "frame_too_large", "script RPC frame too large");
            script_rpc_send_json(srv->listen_fd, err);
            cJSON_Delete(err);
            continue;
         }
         char *buf = calloc(1, (size_t)len + 1);
         if (!buf || script_rpc_read_all(srv->listen_fd, buf, len) != 0)
         {
            free(buf);
            continue;
         }
         cJSON *req = cJSON_Parse(buf);
         cJSON *resp = req ? script_rpc_dispatch(srv, req)
                           : script_rpc_error(0, "bad_json", "invalid script RPC JSON");
         if (resp)
         {
            script_rpc_send_json(srv->listen_fd, resp);
            cJSON_Delete(resp);
         }
         cJSON_Delete(req);
         free(buf);
      }
      return NULL;
   }

   while (!srv->stop)
   {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(srv->listen_fd, &rfds);
      struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
      int sel = select(srv->listen_fd + 1, &rfds, NULL, NULL, &tv);
      if (sel < 0 && errno == EINTR)
         continue;
      if (sel <= 0)
         continue;

      int fd = accept(srv->listen_fd, NULL, NULL);
      if (fd < 0)
         continue;
      uint32_t nlen = 0;
      if (script_rpc_read_all(fd, &nlen, sizeof(nlen)) != 0)
      {
         close(fd);
         continue;
      }
      uint32_t len = ntohl(nlen);
      if (len == 0 || len > SCRIPT_RPC_MAX_FRAME)
      {
         cJSON *err = script_rpc_error(0, "frame_too_large", "script RPC frame too large");
         script_rpc_send_json(fd, err);
         cJSON_Delete(err);
         close(fd);
         continue;
      }
      char *buf = calloc(1, (size_t)len + 1);
      if (!buf || script_rpc_read_all(fd, buf, len) != 0)
      {
         free(buf);
         close(fd);
         continue;
      }
      cJSON *req = cJSON_Parse(buf);
      cJSON *resp = req ? script_rpc_dispatch(srv, req)
                        : script_rpc_error(0, "bad_json", "invalid script RPC JSON");
      if (resp)
      {
         script_rpc_send_json(fd, resp);
         cJSON_Delete(resp);
      }
      cJSON_Delete(req);
      free(buf);
      close(fd);
   }
   return NULL;
}

static int script_rpc_write_file(const char *path, const char *body, mode_t mode)
{
   int fd = open(path, O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY, mode);
   if (fd < 0)
      return -1;
   int rc = script_rpc_write_all(fd, body, strlen(body));
   close(fd);
   return rc;
}

static int script_rpc_write_stubs(script_rpc_server_t *srv)
{
   char py[MAX_PATH_LEN];
   char sh[MAX_PATH_LEN];
   snprintf(py, sizeof(py), "%s/aimee_tools.py", srv->stub_dir);
   snprintf(sh, sizeof(sh), "%s/aimee", srv->stub_dir);

   const char *py_body =
       "import json, os, struct\n"
       "_seq = 0\n"
       "def call(tool, args=None):\n"
       "    global _seq\n"
       "    _seq += 1\n"
       "    args = args or {}\n"
       "    args_json = json.dumps(args, separators=(',', ':'), sort_keys=False)\n"
       "    frame = json.dumps({'id': _seq, 'tool': tool, 'args': args}, "
       "separators=(',', ':')).encode()\n"
       "    fd = os.dup(int(os.environ['AIMEE_RPC_FD']))\n"
       "    os.write(fd, struct.pack('!I', len(frame)) + frame)\n"
       "    hdr = os.read(fd, 4)\n"
       "    if len(hdr) != 4:\n"
       "        os.close(fd)\n"
       "        raise RuntimeError('short RPC response')\n"
       "    size = struct.unpack('!I', hdr)[0]\n"
       "    data = b''\n"
       "    while len(data) < size:\n"
       "        chunk = os.read(fd, size - len(data))\n"
       "        if not chunk:\n"
       "            os.close(fd)\n"
       "            raise RuntimeError('short RPC body')\n"
       "        data += chunk\n"
       "    os.close(fd)\n"
       "    return json.loads(data.decode())\n";
   if (script_rpc_write_file(py, py_body, 0600) != 0)
      return -1;

   const char *sh_body = "#!/usr/bin/env bash\n"
                         "if [ \"$1\" != \"tool\" ] || [ -z \"$2\" ]; then\n"
                         "  echo 'usage: aimee tool <name> [json-args]' >&2; exit 2\n"
                         "fi\n"
                         "tool=\"$2\"; shift 2; args=\"${*:-{}}\"\n"
                         "AIMEE_TOOL_NAME=\"$tool\" AIMEE_TOOL_ARGS=\"$args\" python3 - <<'PY'\n"
                         "import json, os\n"
                         "import aimee_tools\n"
                         "tool = os.environ['AIMEE_TOOL_NAME']\n"
                         "_raw = os.environ.get('AIMEE_TOOL_ARGS', '{}').strip()\n"
                         "args = json.JSONDecoder().raw_decode(_raw)[0]\n"
                         "print(json.dumps(aimee_tools.call(tool, args), separators=(',', ':')))\n"
                         "PY\n";
   return script_rpc_write_file(sh, sh_body, 0700);
}

int script_rpc_prepare(script_rpc_server_t *srv)
{
   if (!srv)
      return -1;
   memset(srv, 0, sizeof(*srv));
   srv->listen_fd = -1;
   srv->client_fd = -1;
   char suffix[17];
   if (platform_random_hex(suffix, 16) != 0)
      return -1;
   snprintf(srv->stub_dir, sizeof(srv->stub_dir), "/tmp/aimee-rpc-%ld-%s", (long)getpid(), suffix);
   if (mkdir(srv->stub_dir, 0700) != 0)
      return -1;
   if (script_rpc_write_stubs(srv) != 0)
      return -1;
   /* The inherited socketpair descriptor is the capability. It replaces the
    * old pathname socket + HMAC token, so scripts receive no credential through
    * argv, env, or a file and unrelated local processes have nothing to guess. */
   int sv[2];
   if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
      return -1;
   srv->listen_fd = sv[0];
   srv->client_fd = sv[1];
   srv->use_fd_pair = 1;
   int flags = fcntl(srv->listen_fd, F_GETFD);
   if (flags >= 0)
      fcntl(srv->listen_fd, F_SETFD, flags | FD_CLOEXEC);
   flags = fcntl(srv->client_fd, F_GETFD);
   if (flags >= 0)
      fcntl(srv->client_fd, F_SETFD, flags & ~FD_CLOEXEC);
   return 0;
}

int script_rpc_start(script_rpc_server_t *srv)
{
   if (!srv || srv->listen_fd < 0)
      return -1;
   if (pthread_create(&srv->thread, NULL, script_rpc_thread, srv) != 0)
      return -1;
   srv->thread_started = 1;
   return 0;
}

void script_rpc_stop(script_rpc_server_t *srv)
{
   if (!srv)
      return;
   srv->stop = 1;
   if (srv->thread_started)
      pthread_join(srv->thread, NULL);
   srv->thread_started = 0;
}

void script_rpc_cleanup(script_rpc_server_t *srv)
{
   if (!srv)
      return;
   if (srv->listen_fd >= 0)
   {
      close(srv->listen_fd);
      srv->listen_fd = -1;
   }
   if (srv->client_fd >= 0)
   {
      close(srv->client_fd);
      srv->client_fd = -1;
   }
   if (srv->stub_dir[0])
   {
      char py[MAX_PATH_LEN];
      char sh[MAX_PATH_LEN];
      snprintf(py, sizeof(py), "%s/aimee_tools.py", srv->stub_dir);
      snprintf(sh, sizeof(sh), "%s/aimee", srv->stub_dir);
      unlink(py);
      unlink(sh);
      rmdir(srv->stub_dir);
   }
}
