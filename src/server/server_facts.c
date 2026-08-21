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

/* All three are gated on CAP_MEMORY_WRITE, matching memory.supersede — the
 * closest analogue, since none of them destroys a row: retraction stamps or
 * tombstones (the row is retained and auditable) and unmerge flips an audit
 * flag. Delete's tier would misdescribe what they do. */
cJSON *facts_retract_command(cJSON *req, attested_transport_t transport)
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

   /* The body's `authority` REQUESTS a level; the connection's attestation grants
    * it. "user" is honoured only when the transport attested a person, so the
    * documented default stays "model" (least privilege: naming no authority never
    * escalates) and naming "user" without having earned it no longer does either.
    * This is the whole difference between a declared identity and an
    * authenticated one. */
   int wants_user = cJSON_IsString(jauth) && strcmp(jauth->valuestring, "user") == 0;
   const char *authority = (wants_user && server_attested_is_person(transport)) ? "user" : "model";

   int retracted = 0;
   int immutable = 0;
   if (kb_client_facts_retract(jsrc->valuestring, jrel->valuestring,
                               cJSON_IsString(jtgt) ? jtgt->valuestring : NULL, authority,
                               &retracted, &immutable) != 0)
   {
      if (immutable)
         return server_error_kind_json(
             SERVER_ERR_INVALID_ARGUMENT,
             "this relation is immutable; only a user authority may retract it", NULL);
      return server_error_kind_json(SERVER_ERR_NOT_FOUND,
                                    "the knowledge service refused the retraction", NULL);
   }

   cJSON *resp = jo_ok();
   /* Reported rather than folded into the status: retracting a fact that was
    * already gone succeeds, and a caller correcting a mistake needs to know
    * whether anything actually changed. */
   cJSON_AddNumberToObject(resp, "retracted", retracted);
   return resp;
}

int handle_facts_retract(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, facts_retract_command(req, conn->attested_transport));
}

cJSON *entities_merge_command(cJSON *req)
{
   int64_t from_id = 0;
   int64_t into_id = 0;
   if (memory_request_positive_id(req, "from_id", &from_id) != 0)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "entities.merge requires a positive integer from_id", NULL);
   if (memory_request_positive_id(req, "into_id", &into_id) != 0)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "entities.merge requires a positive integer into_id", NULL);
   if (from_id == into_id)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "entities.merge cannot merge an entity into itself", NULL);

   int64_t merge_id = 0;
   if (kb_client_entities_merge(from_id, into_id, &merge_id) != 0)
      return server_error_kind_json(
          SERVER_ERR_NOT_FOUND, "merge refused: both ids must be distinct active entities", NULL);

   cJSON *resp = jo_ok();
   /* The audit id is what makes the merge reversible. Returning it is the whole
    * difference between "reversible in principle" and "reversible". */
   cJSON_AddNumberToObject(resp, "merge_id", (double)merge_id);
   return resp;
}

int handle_entities_merge(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, entities_merge_command(req));
}

cJSON *entities_unmerge_command(cJSON *req)
{
   int64_t merge_id = 0;
   if (memory_request_positive_id(req, "merge_id", &merge_id) != 0)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "entities.unmerge requires a positive integer merge_id", NULL);

   if (kb_client_entities_unmerge(merge_id) != 0)
      return server_error_kind_json(SERVER_ERR_NOT_FOUND, "no such merge, or it was already undone",
                                    NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "merge_id", (double)merge_id);
   cJSON_AddBoolToObject(resp, "undone", 1);
   return resp;
}

int handle_entities_unmerge(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, entities_unmerge_command(req));
}
