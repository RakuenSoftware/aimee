/* server_facts.c: server handlers for the typed-fact correction surface.
 *
 * facts.retract (§4) and entities.merge / entities.unmerge (§3): the half of the
 * typed-fact layer that lets a wrong belief be withdrawn. Every primitive here
 * existed and was unit-tested, and none of them had a production caller — the
 * store could learn a fact and nothing above db2 could tell it the fact was
 * wrong, and a mistaken entity merge was reversible only from a test.
 *
 * Split out of server_state.c, which is at its line ceiling. */
#include "server_state_internal.h" /* memory_request_positive_id */
#include "aimee.h"
#include "server.h"
#include "json_fluent.h" /* jo_ok */
#include "kb_client.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>

/* All three are gated on CAP_MEMORY_WRITE, matching memory.supersede — the
 * closest analogue, since none of them destroys a row: retraction stamps or
 * tombstones (the row is retained and auditable) and unmerge flips an audit
 * flag. Delete's tier would misdescribe what they do. */
cJSON *facts_retract_command(cJSON *req, const char *account)
{
   cJSON *jsrc = cJSON_GetObjectItemCaseSensitive(req, "source");
   cJSON *jrel = cJSON_GetObjectItemCaseSensitive(req, "relation");
   cJSON *jtgt = cJSON_GetObjectItemCaseSensitive(req, "target");
   cJSON *jauth = cJSON_GetObjectItemCaseSensitive(req, "authority");

   if (!cJSON_IsString(jsrc) || !jsrc->valuestring[0])
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "facts.retract requires a non-empty source", NULL);
   if (!cJSON_IsString(jrel) || !jrel->valuestring[0])
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "facts.retract requires a non-empty relation", NULL);
   if (jtgt && !cJSON_IsString(jtgt))
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "facts.retract target must be a string", NULL);
   if (jauth && (!cJSON_IsString(jauth) || (strcmp(jauth->valuestring, "user") != 0 &&
                                            strcmp(jauth->valuestring, "model") != 0)))
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "facts.retract authority must be \"user\" or \"model\"", NULL);

   /* The body's `authority` REQUESTS a level; the request's authenticated ACCOUNT
    * grants it. "user" is honoured only when ingress proved an account, so the
    * documented default stays "model" (least privilege: naming no authority never
    * escalates) and naming "user" without having earned it no longer does either.
    * This is the whole difference between a declared identity and an
    * authenticated one. */
   int wants_user = cJSON_IsString(jauth) && strcmp(jauth->valuestring, "user") == 0;
   const char *authority = (wants_user && server_account_is_person(account)) ? "user" : "model";

   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "source", jsrc->valuestring);
   cJSON_AddStringToObject(request, "relation", jrel->valuestring);
   cJSON_AddStringToObject(request, "target", cJSON_IsString(jtgt) ? jtgt->valuestring : "");
   cJSON_AddStringToObject(request, "authority", authority);
   char *raw = kb_v1_action_request("facts.retract", request);
   cJSON *response = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   const char *status = jo_cstr(response, "status");
   int ok = strcmp(status, "ok") == 0;
   const cJSON *count = cJSON_GetObjectItemCaseSensitive(response, "retracted");
   int valid = cJSON_IsObject(response) && ((!ok && strcmp(status, "error") == 0) ||
                                            (ok && cJSON_IsNumber(count) && count->valueint >= 0 &&
                                             count->valuedouble == (double)count->valueint));
   kb_client_memory_audit_note("facts.retract", 0, NULL, jrel->valuestring, NULL, 0.0, NULL,
                               valid && ok);
   if (!valid)
   {
      cJSON_Delete(response);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                    "fact retraction unavailable or invalid response", NULL);
   }
   if (!ok)
   {
      const char *kind = jo_cstr(response, "kind");
      char *owned_kind = strdup(kind[0] ? kind : SERVER_ERR_UNAVAILABLE);
      if (!owned_kind)
      {
         cJSON_Delete(response);
         return server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                       "fact retraction response unavailable", NULL);
      }
      server_error_kind_apply(response, owned_kind);
      free(owned_kind);
   }
   return response;
}

int handle_facts_retract(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, facts_retract_command(req, server_request_account()));
}

/* Preserve the complete Go-owned reply, including exact integer tokens. */
static cJSON *entities_command(const char *method, cJSON *req)
{
   cJSON *request = cJSON_CreateObject();
   const char *fields[] = {"from_id", "into_id", "merge_id"};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(req, fields[i]);
      if (value)
         cJSON_AddItemToObject(request, fields[i], cJSON_Duplicate(value, 1));
   }
   char *raw = request ? kb_v1_action_request(method, request) : NULL;
   cJSON *parsed = raw && strlen(raw) <= 1048576 ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
   const char *status = jo_cstr(parsed, "status");
   cJSON *result = NULL;
   cJSON *id = cJSON_GetObjectItemCaseSensitive(parsed, "merge_id");
   int valid_id = cJSON_IsNumber(id) && isfinite(id->valuedouble) && id->valuedouble > 0 &&
                  trunc(id->valuedouble) == id->valuedouble;
   if (cJSON_IsObject(parsed) && !strcmp(status, "ok") && valid_id &&
       jo_cstr(parsed, "commit_id")[0])
      result = cJSON_CreateRaw(raw);
   else if (cJSON_IsObject(parsed) && !strcmp(status, "error"))
   {
      const char *kind = jo_cstr(parsed, "kind");
      char *owned = strdup(kind[0] ? kind : SERVER_ERR_UNAVAILABLE);
      if (owned)
      {
         server_error_kind_apply(parsed, owned);
         free(owned);
         result = parsed;
         parsed = NULL;
      }
   }
   cJSON_Delete(parsed);
   free(raw);
   return result
              ? result
              : server_error_kind_json(SERVER_ERR_UNAVAILABLE, "entity mutation unavailable", NULL);
}

cJSON *entities_merge_command(cJSON *req)
{
   return entities_command("entities.merge", req);
}
cJSON *entities_unmerge_command(cJSON *req)
{
   return entities_command("entities.unmerge", req);
}

int handle_entities_merge(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, entities_merge_command(req));
}
int handle_entities_unmerge(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, entities_unmerge_command(req));
}
