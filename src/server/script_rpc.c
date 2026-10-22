#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "script_rpc.h"
#include "script_tool_execution.h"
#include "server_session_control.h"
#include "server_session_capture.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/tools/agent_tools.h>
#include "aimee.h"
#include "cJSON.h"
#include "platform_random.h"
#include "toolset.h"
#include "agent_config.h"
#include "agent_source_authority.h"
#include "agent_tasks.h"
#include "request_context.h"
#include "modules/workspace/workspace_provider.h"
#include "server.h"
#include "util.h"
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

#define SCRIPT_RPC_MAX_FRAME (64 * 1024)
/* A result can appear twice (result/error), and each byte may need six JSON
 * escape bytes. Reserve envelope space even for that worst-case projection. */
#define SCRIPT_RPC_PREVIEW_BYTES ((SCRIPT_RPC_MAX_FRAME - 1024) / 12)

struct script_rpc_execution
{
   server_control_scope_t *controls;
   server_session_capture_t *capture;
   agent_tools_scope_snapshot_t tools;
   request_context_snapshot_t request;
   agent_source_authority_snapshot_t source;
   const workspace_provider_t *workspace;
   atomic_int *cancel;
   agent_request_cancel_scope_t cancellation;
   char cwd[MAX_PATH_LEN], session[128], principal[VAULT_PRINCIPAL_MAX];
   int job;
   char attempt_prefix[33];
   unsigned long long attempts;
};

static int script_rpc_capture(script_rpc_server_t *srv, const char *cwd)
{
   struct script_rpc_execution *scope = calloc(1, sizeof *scope);
   srv->execution = scope;
   if (!scope || (cwd && strlen(cwd) >= sizeof scope->cwd) ||
       agent_tools_scope_capture_owned(&scope->tools) != 0 ||
       agent_source_authority_tls_capture_checked(&scope->source) != 0 ||
       platform_random_hex(scope->attempt_prefix, 32) != 0)
      return -1;
   /* An ordinary primary must not borrow a concurrent delegate's environment. */
   if (!scope->source.active)
      scope->source.active = 1;
   request_context_snapshot(&scope->request);
   if (scope->request.active)
   {
      scope->request.value.capabilities &= CAPS_AUTHENTICATED;
   }
   const char *session = session_id(), *principal = agent_get_request_vault_principal();
   snprintf(scope->cwd, sizeof scope->cwd, "%s", cwd ? cwd : "");
   snprintf(scope->session, sizeof scope->session, "%s", session ? session : "");
   snprintf(scope->principal, sizeof scope->principal, "%s", principal ? principal : "");
   const char *actor =
       scope->request.active ? request_context_session_actor(&scope->request.value) : "";
   if (server_session_capture_retain_current(scope->session, actor, &scope->capture))
      return -1;
   if (scope->request.active)
   {
      request_context_t *owner = &scope->request.value;
      int direct_operator =
          !owner->model_execution && actor[0] && (owner->capabilities & CAP_TOOL_EXECUTE);
      owner->operator_script_origin =
          !scope->capture && (direct_operator || owner->operator_script_origin);
      if (owner->model_execution && !owner->operator_script_origin && !scope->capture)
         return -1;
      owner->model_execution = 1;
   }
   if (server_control_scope_capture(scope->session, scope->principal, &scope->controls))
      return -1;
   scope->workspace = workspace_provider_active();
   scope->cancel = agent_get_request_cancel();
   scope->cancellation.flag = &srv->stop;
   scope->cancellation.parent = agent_request_cancel_scope_current();
   scope->job = agent_get_durable_job_id();
   return 0;
}

static void script_rpc_release(script_rpc_server_t *srv)
{
   struct script_rpc_execution *scope = srv->execution;
   if (!scope)
      return;
   agent_tools_scope_destroy_owned(&scope->tools);
   agent_source_authority_snapshot_destroy(&scope->source);
   request_context_snapshot_clear(&scope->request);
   server_control_scope_release(scope->controls);
   server_session_capture_release(scope->capture);
   free(scope);
   srv->execution = NULL;
}

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

/* A disconnected script must never deliver SIGPIPE to its host. */
static int script_rpc_send_all(int fd, const void *buffer, size_t length)
{
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
   int enabled = 1;
   if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) != 0)
      return -1;
#endif
   const char *bytes = buffer;
   while (length)
   {
      ssize_t sent = -1;
#ifdef MSG_NOSIGNAL
      sent = send(fd, bytes, length, MSG_NOSIGNAL);
#elif defined(SO_NOSIGPIPE)
      sent = send(fd, bytes, length, 0);
#else
      return -1;
#endif
      if (sent < 0 && errno == EINTR)
         continue;
      if (sent <= 0)
         return -1;
      bytes += sent;
      length -= (size_t)sent;
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
   int rc = len <= SCRIPT_RPC_MAX_FRAME && script_rpc_send_all(fd, &nlen, sizeof(nlen)) == 0 &&
                    script_rpc_send_all(fd, text, len) == 0
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
   cJSON *encoded = cJSON_GetObjectItemCaseSensitive(req, "args_json");
   int id = cJSON_IsNumber(jid) ? jid->valueint : 0;
   if (encoded)
   {
      if (jargs || !cJSON_IsString(encoded))
         return script_rpc_error(id, "bad_frame", "ambiguous script tool arguments");
      cJSON *decoded =
          aimee_ir_json_parse_frame(encoded->valuestring, strlen(encoded->valuestring));
      if (!decoded)
         return script_rpc_error(id, "bad_json", "invalid script tool arguments");
      if (!cJSON_IsObject(decoded))
      {
         cJSON_Delete(decoded);
         return script_rpc_error(id, "bad_frame", "script tool requires object arguments");
      }
      if (!cJSON_AddItemToObject(req, "args", decoded))
      {
         cJSON_Delete(decoded);
         return script_rpc_error(id, "internal", "cannot admit script tool arguments");
      }
      cJSON_DeleteItemFromObjectCaseSensitive(req, "args_json");
      jargs = decoded;
   }
   if (!cJSON_IsNumber(jid) || !cJSON_IsString(jtool) || !cJSON_IsObject(jargs))
      return script_rpc_error(id, "bad_frame", "missing id, tool, or args");

   if (agent_request_cancelled())
      return script_rpc_error(id, "cancelled", "script tool execution cancelled");

   if (!script_rpc_allowed_tool(jtool->valuestring))
   {
      srv->policy_denials++;
      return script_rpc_error(id, "policy_denied", "tool is not allowed through script RPC");
   }

   char attempt[96];
   snprintf(attempt, sizeof attempt, "script-rpc:%s:%llu", srv->execution->attempt_prefix,
            ++srv->execution->attempts);
   int is_error = 1;
   char *result = server_script_tool_execute(req, attempt, &is_error);
   srv->tool_calls++;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddNumberToObject(resp, "id", id);
   cJSON_AddBoolToObject(resp, "ok", !is_error);
   if (result)
   {
      size_t bytes = strlen(result);
      int projected = bytes > SCRIPT_RPC_MAX_FRAME;
      if (!projected)
      {
         /* A valid prefix is not the result. Preserve ambiguous/non-JSON tool
          * text verbatim rather than silently discarding trailing content. */
         cJSON *parsed = aimee_ir_json_parse_frame(result, bytes);
         if (parsed)
            cJSON_AddItemToObject(resp, "result", parsed);
         else
            cJSON_AddStringToObject(resp, "result", result);
         if (is_error)
            cJSON_AddStringToObject(resp, "error", result);
         char *encoded = cJSON_PrintUnformatted(resp);
         projected = !encoded || strlen(encoded) > SCRIPT_RPC_MAX_FRAME;
         free(encoded);
      }
      if (projected)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(resp, "result");
         cJSON_DeleteItemFromObjectCaseSensitive(resp, "error");
         size_t prefix = bytes < SCRIPT_RPC_PREVIEW_BYTES ? bytes : SCRIPT_RPC_PREVIEW_BYTES;
         /* Do not split a UTF-8 character in an otherwise valid text result. */
         while (prefix && ((unsigned char)result[prefix] & 0xc0) == 0x80)
            prefix--;
         result[prefix] = 0;
         cJSON_AddStringToObject(resp, "result", result);
         if (is_error)
            cJSON_AddStringToObject(resp, "error", result);
         cJSON_AddBoolToObject(resp, "result_truncated", 1);
         cJSON_AddNumberToObject(resp, "result_bytes", (double)bytes);
      }
   }
   else
   {
      cJSON_AddStringToObject(resp, "error", "tool returned no result");
   }
   free(result);
   return resp;
}

static void *script_rpc_serve(void *arg)
{
   script_rpc_server_t *srv = (script_rpc_server_t *)arg;
   if (srv->use_fd_pair)
   {
      while (!atomic_load(&srv->stop))
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
         cJSON *req = aimee_ir_json_parse_frame(buf, len);
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

   while (!atomic_load(&srv->stop))
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
      cJSON *req = aimee_ir_json_parse_frame(buf, len);
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

static void *script_rpc_thread(void *arg)
{
   script_rpc_server_t *srv = arg;
   struct script_rpc_execution *scope = srv->execution;
   server_control_scope_t *previous_controls = server_control_scope_bind(scope->controls);
   server_session_capture_t *previous_capture = server_session_capture_bind(scope->capture);
   agent_tools_scope_restore(&scope->tools);
   request_context_restore(&scope->request);
   agent_source_authority_tls_restore(&scope->source);
   workspace_provider_set_active(scope->workspace);
   run_cmd_set_cwd(scope->cwd[0] ? scope->cwd : NULL);
   agent_set_request_session(scope->session);
   agent_set_request_vault_principal(scope->principal);
   session_id_set_override(scope->session);
   agent_set_request_cancel(scope->cancel);
   agent_request_cancel_scope_bind(&scope->cancellation);
   agent_set_durable_job(scope->job);
   script_rpc_serve(arg);
   /* The caller's resource lease still encloses this entire joined worker. */
   agent_source_authority_snapshot_t empty_source = {.active = 1};
   agent_source_authority_tls_restore(&empty_source);
   agent_tools_scope_snapshot_t empty_tools = {0};
   agent_tools_scope_restore(&empty_tools);
   workspace_provider_set_active(NULL);
   run_cmd_set_cwd(NULL);
   session_id_clear_override();
   agent_set_request_session(NULL);
   agent_set_request_vault_principal(NULL);
   agent_set_request_cancel(NULL);
   agent_request_cancel_scope_bind(NULL);
   agent_set_durable_job(0);
   request_context_clear();
   server_control_scope_bind(previous_controls);
   server_session_capture_bind(previous_capture);
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

   const char *py_body = "import json, os, struct\n"
                         "_seq = 0\n"
                         "def _read_exact(fd, size):\n"
                         "    data = b''\n"
                         "    while len(data) < size:\n"
                         "        chunk = os.read(fd, size - len(data))\n"
                         "        if not chunk:\n"
                         "            raise RuntimeError('short RPC response')\n"
                         "        data += chunk\n"
                         "    return data\n"
                         "def _exchange(tool, args, encoded=False):\n"
                         "    global _seq\n"
                         "    _seq += 1\n"
                         "    field = 'args_json' if encoded else 'args'\n"
                         "    frame = json.dumps({'id': _seq, 'tool': tool, field: args}, "
                         "separators=(',', ':'), sort_keys=False).encode()\n"
                         "    if not frame or len(frame) > 65536:\n"
                         "        raise ValueError('RPC frame exceeds limit')\n"
                         "    fd = os.dup(int(os.environ['AIMEE_RPC_FD']))\n"
                         "    try:\n"
                         "        packet = memoryview(struct.pack('!I', len(frame)) + frame)\n"
                         "        while packet:\n"
                         "            sent = os.write(fd, packet)\n"
                         "            if sent <= 0:\n"
                         "                raise RuntimeError('short RPC write')\n"
                         "            packet = packet[sent:]\n"
                         "        size = struct.unpack('!I', _read_exact(fd, 4))[0]\n"
                         "        if not 0 < size <= 65536:\n"
                         "            raise RuntimeError('invalid RPC response size')\n"
                         "        return _read_exact(fd, size)\n"
                         "    finally:\n"
                         "        os.close(fd)\n"
                         "def call(tool, args=None):\n"
                         "    args = {} if args is None else args\n"
                         "    return json.loads(_exchange(tool, args).decode())\n";
   if (script_rpc_write_file(py, py_body, 0600) != 0)
      return -1;

   const char *sh_body = "#!/usr/bin/env bash\n"
                         "if [ \"$1\" != \"tool\" ] || [ -z \"$2\" ]; then\n"
                         "  echo 'usage: aimee tool <name> [json-args]' >&2; exit 2\n"
                         "fi\n"
                         "tool=\"$2\"; shift 2\n"
                         "if [ \"$#\" -eq 0 ]; then args='{}'; else args=\"$*\"; fi\n"
                         "printf '%s\\0%s' \"$tool\" \"$args\" | python3 -c '\n"
                         "import sys\n"
                         "import aimee_tools\n"
                         "tool_raw, args_raw = sys.stdin.buffer.read().split(b\"\\0\", 1)\n"
                         "tool = tool_raw.decode()\n"
                         "sys.stdout.buffer.write(aimee_tools._exchange(tool, args_raw.decode(), "
                         "encoded=True) + b\"\\n\")\n"
                         "'\n";
   return script_rpc_write_file(sh, sh_body, 0700);
}

int script_rpc_prepare(script_rpc_server_t *srv, const char *cwd)
{
   if (!srv)
      return -1;
   memset(srv, 0, sizeof(*srv));
   srv->listen_fd = -1;
   srv->client_fd = -1;
   atomic_init(&srv->stop, 0);
   if (script_rpc_capture(srv, cwd) != 0)
      return -1;
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
   if (!srv || srv->listen_fd < 0 || !srv->execution)
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
   atomic_store(&srv->stop, 1);
   if (srv->thread_started)
   {
      shutdown(srv->listen_fd, SHUT_RDWR);
      pthread_join(srv->thread, NULL);
   }
   srv->thread_started = 0;
}

void script_rpc_cleanup(script_rpc_server_t *srv)
{
   if (!srv)
      return;
   script_rpc_stop(srv);
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
   script_rpc_release(srv);
}
