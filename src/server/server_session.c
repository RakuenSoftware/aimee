/* server_session.c: session lifecycle handlers */
#include "aimee.h"
#include "db1.h"
#include "server.h"
#include "log.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "presence.h"
#include "turn_registry.h"
#include "git_verify.h"
#include "kb_client.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   if (!sid || !sid[0])
      return server_send_error(conn, "missing session_id", NULL);

   /* Generate episode card before removing the session record.  aimee-kb
    * loads its own config and short-circuits if summarisation is disabled. */
   int64_t uid = kb_client_memory_episode_card_generate(sid);
   if (uid <= 0)
      aimee_log(LOG_WARN, "session_close", "episode card generation produced no row for session %s",
                sid);

   if (db1_server_session_delete(sid) != 0)
      return server_send_error(conn, "failed to close session", NULL);

   (void)verify_cancel_session(sid);
   /* Cancel any in-flight turn for this session before tearing down its
    * presence: a turn now outlives its client connection, so closing the
    * session is what stops it (server-owned turn lifecycle). NULL owner = a
    * trusted internal caller, bypassing the cross-principal check. */
   (void)turn_registry_cancel(sid, NULL);
   server_session_pool_close(ctx, sid);
   /* Tear down the in-process presence so it doesn't outlive the session, and
    * any open /v1/sessions/<id>/events SSE streams terminate (GONE). */
   (void)presence_session_close(sid);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "session_id", sid);
   return server_send_ok(conn, resp);
}

/* chat.graceful_cancel: stop an in-flight turn by session id without tearing the
 * session down (server-owned turn lifecycle — also backs the gateway `/stop`,
 * which was previously a no-op). Authorized against the session's presence owner:
 * an attested caller (conn->vault_principal set) must match it; a trusted local
 * peer (un-attested, empty principal over the filesystem-trusted UDS) is allowed,
 * with the forwarding surface (webchat login / gateway pairing) binding session
 * id to the caller. */
int handle_chat_graceful_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "aimee_session_id");
   if (!cJSON_IsString(jsid))
      jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *sid = (jsid && cJSON_IsString(jsid)) ? jsid->valuestring : NULL;
   if (!sid || !sid[0])
      return server_send_error(conn, "missing aimee_session_id", NULL);
   int rc = turn_registry_cancel(sid, conn->vault_principal);
   if (rc < 0)
      return server_send_error(conn, "forbidden: not the session owner", NULL);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "session_id", sid);
   cJSON_AddBoolToObject(resp, "cancelled", rc == 1 ? 1 : 0);
   return server_send_ok(conn, resp);
}

/* session.attach: register a surface as an attachment of a presence (creating
 * the presence on first attach), per the unified-presence model. Mirrors the
 * REST POST /v1/sessions/{id}/attach, for the NDJSON/CLI transport. */
int handle_session_attach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *sid, *surface;
   if (jo_need_str(req, "session_id", &sid) < 0 || jo_need_str(req, "surface", &surface) < 0)
      return server_send_error(conn, "missing session_id or surface", NULL);

   const cJSON *jo = cJSON_GetObjectItemCaseSensitive(req, "owner");
   const char *owner = (cJSON_IsString(jo) && jo->valuestring[0]) ? jo->valuestring : NULL;
   const cJSON *jt = cJSON_GetObjectItemCaseSensitive(req, "target");
   const char *target = (cJSON_IsString(jt) && jt->valuestring[0]) ? jt->valuestring : NULL;
   const cJSON *jm = cJSON_GetObjectItemCaseSensitive(req, "subscribe_mask");
   unsigned mask = cJSON_IsNumber(jm) ? (unsigned)jm->valuedouble : (unsigned)PRESENCE_EV_ALL;
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "persistent");
   int persistent = cJSON_IsBool(jp) ? cJSON_IsTrue(jp) : 0;

   char attach_id[64];
   if (!presence_attach(sid, owner, surface, target, mask, persistent, attach_id,
                        sizeof(attach_id)))
      return server_send_error(conn, "attach refused (owner mismatch or registry full)", NULL);

   cJSON *resp = jo_ok();
   jo_add_str(resp, "session_id", sid);
   jo_add_str(resp, "attach_id", attach_id);
   return server_send_ok(conn, resp);
}

/* session.detach: drop an attachment from a presence. */
int handle_session_detach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *sid, *aid;
   if (jo_need_str(req, "session_id", &sid) < 0 || jo_need_str(req, "attach_id", &aid) < 0)
      return server_send_error(conn, "missing session_id or attach_id", NULL);

   int detached = presence_detach(sid, aid);
   cJSON *resp = jo_ok();
   jo_add_str(resp, "session_id", sid);
   jo_add_bool(resp, "detached", detached);
   return server_send_ok(conn, resp);
}

/* session.presence: list the owner's live presences (the NDJSON/CLI twin of
 * the REST GET /v1/sessions). An optional "owner" filters the list. */
int handle_session_presence(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const cJSON *jo = cJSON_GetObjectItemCaseSensitive(req, "owner");
   const char *owner = (cJSON_IsString(jo) && jo->valuestring[0]) ? jo->valuestring : NULL;

   char buf[8192];
   presence_list_json(owner, buf, sizeof(buf));
   cJSON *arr = cJSON_Parse(buf);

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "presences", arr ? arr : cJSON_CreateArray());
   return server_send_ok(conn, resp);
}

static int session_brief_sid_valid(const char *sid)
{
   if (!sid || !sid[0])
      return 0;
   for (const unsigned char *p = (const unsigned char *)sid; *p; p++)
   {
      if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.'))
         return 0;
   }
   return 1;
}

static int session_brief_filename_sid(const char *name, char *sid, size_t sid_len)
{
   if (strncmp(name, "session-", 8) != 0)
      return 0;

   const char *suffix = strstr(name, ".brief.md");
   if (!suffix || suffix[9] != '\0')
      return 0;

   size_t n = (size_t)(suffix - (name + 8));
   if (n == 0 || n >= sid_len)
      return 0;

   memcpy(sid, name + 8, n);
   sid[n] = '\0';
   return session_brief_sid_valid(sid);
}

static void session_brief_mtime_utc(time_t mtime, char *out, size_t out_len)
{
   struct tm tm_buf;
   gmtime_r(&mtime, &tm_buf);
   strftime(out, out_len, "%Y-%m-%d %H:%M UTC", &tm_buf);
}

static char *session_brief_latest_sid(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return NULL;

   char best_sid[128] = "";
   time_t best_mtime = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      char sid[128];
      if (!session_brief_filename_sid(ent->d_name, sid, sizeof(sid)))
         continue;

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
         continue;

      if (st.st_mtime > best_mtime)
      {
         best_mtime = st.st_mtime;
         snprintf(best_sid, sizeof(best_sid), "%s", sid);
      }
   }
   closedir(d);

   return best_sid[0] ? strdup(best_sid) : NULL;
}

static int handle_session_brief_list(server_conn_t *conn, const char *dir)
{
   cJSON *briefs = cJSON_CreateArray();
   DIR *d = opendir(dir);
   if (d)
   {
      struct dirent *ent;
      while ((ent = readdir(d)) != NULL)
      {
         char sid[128];
         if (!session_brief_filename_sid(ent->d_name, sid, sizeof(sid)))
            continue;

         char path[MAX_PATH_LEN];
         snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
         struct stat st;
         if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

         char ts[32];
         session_brief_mtime_utc(st.st_mtime, ts, sizeof(ts));

         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "session_id", sid);
         cJSON_AddStringToObject(item, "modified_at", ts);
         cJSON_AddNumberToObject(item, "bytes", (double)st.st_size);
         cJSON_AddItemToArray(briefs, item);
      }
      closedir(d);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "briefs", briefs);
   return server_send_ok(conn, resp);
}

int handle_session_brief(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *dir = config_output_dir();
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "list")))
      return handle_session_brief_list(conn, dir);

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   char *resolved_sid = NULL;
   if (!sid || !sid[0])
   {
      resolved_sid = session_brief_latest_sid(dir);
      sid = resolved_sid;
   }

   if (!sid || !sid[0])
   {
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "message",
                              "No session briefs found. Run `aimee session-start` first.");
      return server_send_ok(conn, resp);
   }
   if (!session_brief_sid_valid(sid))
   {
      free(resolved_sid);
      return server_send_error(conn, "invalid session_id", NULL);
   }

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/session-%s.brief.md", dir, sid);
   FILE *fp = fopen(path, "rb");
   if (!fp)
   {
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "session_id", sid);
      cJSON_AddStringToObject(resp, "message", "No brief found for session.");
      int rc = server_send_response(conn, resp);
      cJSON_Delete(resp);
      free(resolved_sid);
      return rc;
   }

   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      free(resolved_sid);
      return server_send_error(conn, "failed to read brief", NULL);
   }
   long len = ftell(fp);
   if (len < 0)
   {
      fclose(fp);
      free(resolved_sid);
      return server_send_error(conn, "failed to read brief", NULL);
   }
   rewind(fp);

   char *brief = malloc((size_t)len + 1);
   if (!brief)
   {
      fclose(fp);
      free(resolved_sid);
      return server_send_error(conn, "out of memory", NULL);
   }
   size_t n = fread(brief, 1, (size_t)len, fp);
   fclose(fp);
   brief[n] = '\0';

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "session_id", sid);
   cJSON_AddStringToObject(resp, "brief", brief);
   cJSON_AddNumberToObject(resp, "bytes", (double)n);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   free(brief);
   free(resolved_sid);
   return rc;
}
