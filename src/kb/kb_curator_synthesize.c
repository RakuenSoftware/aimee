/* kb_curator_synthesize.c: deep-curator synthesize_topic pass. See header.
 *
 * Topic selection is centrality-first: the committed `entity` with the most
 * inbound `mentions` links that has no `synthesis` written `about` it yet. The
 * synthesis artifact, once written and linked, removes that entity from the
 * candidate set so the pass converges over a corpus instead of re-picking the
 * same hub. No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_synthesize.h"
#include "kb_curator_sidecar.h"
#include "aimee.h"
#include "config.h"
#include "cJSON.h"
#include "log.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <stdlib.h>
#include <string.h>

#define CURATOR_SYNTH_OUTBUF    16384
#define CURATOR_SYNTH_DEFAULT_K 8

int kb_curator_restore_fragment_record(int64_t fragment_doc_id, const char *base_artifact_id,
                                       const char *restored_text, double confidence,
                                       const char *prompt_version, char *artifact_id_out,
                                       size_t artifact_id_len)
{
   if (fragment_doc_id <= 0 || !restored_text || !restored_text[0])
      return -1;

   char fragment_id[32];
   snprintf(fragment_id, sizeof(fragment_id), "%lld", (long long)fragment_doc_id);

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   cJSON_AddStringToObject(payload, "text", restored_text);
   cJSON_AddStringToObject(payload, "evidence_mode", "synthesised");
   cJSON_AddStringToObject(payload, "restoration_prompt_version",
                           prompt_version && prompt_version[0] ? prompt_version : "restore-v1");
   cJSON_AddStringToObject(payload, "fragment_doc_id", fragment_id);
   if (base_artifact_id && base_artifact_id[0])
      cJSON_AddStringToObject(payload, "base_artifact_id", base_artifact_id);
   cJSON_AddBoolToObject(payload, "unknown_sentinel_present",
                         strstr(restored_text, "[unknown]") != NULL);
   cJSON_AddStringToObject(payload, "guardrail",
                           "preserve source nouns, numbers, and relationships; mark gaps "
                           "[unknown]; never invent");
   char *payload_json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);

   char restore_id[64];
   db2_artifact_gen_id(restore_id, sizeof(restore_id));
   int rc = db2_artifact_write(restore_id, "restoration", "committed", "doc", fragment_id,
                               "curator", confidence, payload_json ? payload_json : "{}");
   free(payload_json);
   if (rc != 0)
      return -1;

   if (db2_artifact_cite(restore_id, "doc", fragment_id) != 0)
      return -1;
   if (base_artifact_id && base_artifact_id[0])
   {
      if (db2_artifact_cite(restore_id, "artifact", base_artifact_id) != 0)
         return -1;
      if (db2_artifact_link(restore_id, base_artifact_id, "restored_from") != 0)
         return -1;
      if (db2_artifact_link(base_artifact_id, restore_id, "restores") != 0)
         return -1;
   }

   char audit_id[64];
   db2_artifact_gen_id(audit_id, sizeof(audit_id));
   char before_json[128];
   char after_json[256];
   snprintf(before_json, sizeof(before_json), "{\"fragment_doc_id\":\"%s\"}", fragment_id);
   snprintf(after_json, sizeof(after_json),
            "{\"artifact_id\":\"%s\",\"evidence_mode\":\"synthesised\"}", restore_id);
   if (db2_audit_event_write(audit_id, restore_id, "corpus.restore", fragment_id, "curator", "doc",
                             fragment_id, confidence, 0, before_json, after_json) != 0)
      return -1;

   if (artifact_id_out && artifact_id_len > 0)
      snprintf(artifact_id_out, artifact_id_len, "%s", restore_id);
   return 0;
}

/* Pick the highest-centrality un-synthesized topic entity. Writes id/payload/
 * scope into the caller buffers. Returns 1 if one was found, else 0. */
int kb_curator_synth_pick_topic(void *conn, char *id, size_t id_len, char **payload_out,
                                char *scope_kind, size_t sk_len, char *scope_id, size_t si_len)
{
   static const char *sql =
       "SELECT a.id, a.payload, a.scope_kind, a.scope_id, COUNT(l.from_id) AS deg"
       " FROM artifacts a"
       " JOIN artifact_links l ON l.to_id = a.id AND l.kind = 'mentions'"
       " WHERE a.kind = 'entity' AND a.state = 'committed'"
       "   AND NOT EXISTS ("
       "     SELECT 1 FROM artifact_links s"
       "     JOIN artifacts sa ON sa.id = s.from_id AND sa.kind = 'synthesis'"
       "     WHERE s.to_id = a.id AND s.kind = 'about')"
       " GROUP BY a.id, a.payload, a.scope_kind, a.scope_id"
       " HAVING COUNT(l.from_id) >= 1"
       " ORDER BY deg DESC, a.id ASC"
       " LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_pl = aimee_pg_column_text(st, 1);
      const char *c_sk = aimee_pg_column_text(st, 2);
      const char *c_si = aimee_pg_column_text(st, 3);
      snprintf(id, id_len, "%s", c_id ? c_id : "");
      *payload_out = strdup(c_pl ? c_pl : "{}");
      snprintf(scope_kind, sk_len, "%s", c_sk ? c_sk : "");
      snprintf(scope_id, si_len, "%s", c_si ? c_si : "");
      found = id[0] ? 1 : 0;
   }
   aimee_pg_finalize(st);
   return found;
}

/* Build the synthesize request JSON: {task, topic:{id,name}, sources:[...]}.
 * Each source carries its id, kind and parsed payload. Also appends each source
 * id to `cites` (caller-owned array) for later citation links. Returns a
 * malloc'd request string (caller frees) or NULL; *n_sources gets the count. */
static char *synth_build_request(void *conn, const char *topic_id, const char *topic_name, int k,
                                 cJSON *cites, int *n_sources)
{
   *n_sources = 0;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON_AddStringToObject(root, "task", "synthesize_topic");
   cJSON *topic = cJSON_AddObjectToObject(root, "topic");
   if (topic)
   {
      cJSON_AddStringToObject(topic, "id", topic_id);
      cJSON_AddStringToObject(topic, "name", topic_name ? topic_name : "");
   }
   cJSON *sources = cJSON_AddArrayToObject(root, "sources");

   static const char *sql = "SELECT a2.id, a2.kind, a2.payload"
                            " FROM artifact_links l"
                            " JOIN artifacts a2 ON a2.id = l.from_id"
                            " WHERE l.to_id = :ent AND l.kind = 'mentions'"
                            " ORDER BY a2.id LIMIT :k";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (st && sources)
   {
      aimee_pg_bind_text(st, "ent", topic_id);
      aimee_pg_bind_int(st, "k", k > 0 ? k : CURATOR_SYNTH_DEFAULT_K);
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *s_id = aimee_pg_column_text(st, 0);
         const char *s_kind = aimee_pg_column_text(st, 1);
         const char *s_pl = aimee_pg_column_text(st, 2);
         cJSON *src = cJSON_CreateObject();
         if (!src)
            break;
         cJSON_AddStringToObject(src, "id", s_id ? s_id : "");
         cJSON_AddStringToObject(src, "kind", s_kind ? s_kind : "");
         cJSON *pl = s_pl ? cJSON_Parse(s_pl) : NULL;
         if (pl)
            cJSON_AddItemToObject(src, "payload", pl);
         else
            cJSON_AddStringToObject(src, "payload", s_pl ? s_pl : "{}");
         cJSON_AddItemToArray(sources, src);
         if (cites && s_id && s_id[0])
            cJSON_AddItemToArray(cites, cJSON_CreateString(s_id));
         (*n_sources)++;
      }
   }
   aimee_pg_finalize(st);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json;
}

int kb_curator_synthesize_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_curator_synthesize_enabled || !cfg.kb_curator_synthesize_command[0])
      return 0;

   char ent_id[64] = "", scope_kind[64] = "", scope_id[128] = "";
   char *ent_payload = NULL;
   if (!kb_curator_synth_pick_topic(conn, ent_id, sizeof(ent_id), &ent_payload, scope_kind,
                                    sizeof(scope_kind), scope_id, sizeof(scope_id)))
   {
      free(ent_payload);
      return 0;
   }

   char topic_name[256] = "";
   cJSON *ep = ent_payload ? cJSON_Parse(ent_payload) : NULL;
   if (ep)
   {
      const cJSON *jn = cJSON_GetObjectItemCaseSensitive(ep, "name");
      if (cJSON_IsString(jn) && jn->valuestring[0])
         snprintf(topic_name, sizeof(topic_name), "%s", jn->valuestring);
      cJSON_Delete(ep);
   }
   free(ent_payload);

   cJSON *cites = cJSON_CreateArray();
   int n_sources = 0;
   char *request = synth_build_request(conn, ent_id, topic_name, cfg.kb_curator_synthesize_k, cites,
                                       &n_sources);
   if (!request || n_sources == 0)
   {
      /* Nothing to synthesize from — skip (don't burn the sidecar on no input). */
      free(request);
      cJSON_Delete(cites);
      return 0;
   }

   char serr[256];
   char *response = kb_curator_sidecar_run(cfg.kb_curator_synthesize_command, request,
                                           CURATOR_SYNTH_OUTBUF, serr, sizeof(serr));
   free(request);
   if (!response)
   {
      aimee_log(LOG_WARN, "kb.curator.synth", "synthesize sidecar failed for '%s' (%s)", topic_name,
                serr);
      cJSON_Delete(cites);
      return 0;
   }

   /* Tolerate prose/fence-wrapped output: scan to the first object. */
   const char *start = strchr(response, '{');
   cJSON *resp = start ? cJSON_Parse(start) : NULL;
   free(response);
   const cJSON *text = resp ? cJSON_GetObjectItemCaseSensitive(resp, "synthesis") : NULL;
   if (!cJSON_IsString(text))
      text = resp ? cJSON_GetObjectItemCaseSensitive(resp, "text") : NULL;
   if (!cJSON_IsString(text) || !text->valuestring[0])
   {
      aimee_log(LOG_WARN, "kb.curator.synth", "unparseable synthesis for '%s'", topic_name);
      cJSON_Delete(resp);
      cJSON_Delete(cites);
      return 0;
   }

   /* Build the synthesis artifact payload: text + topic provenance + citations. */
   cJSON *payload = cJSON_CreateObject();
   cJSON_AddStringToObject(payload, "text", text->valuestring);
   cJSON_AddStringToObject(payload, "topic_entity_id", ent_id);
   cJSON_AddStringToObject(payload, "topic_name", topic_name);
   cJSON_AddItemToObject(payload, "citations", cJSON_Duplicate(cites, 1));
   char *payload_json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   cJSON_Delete(resp);

   char synth_id[64];
   db2_artifact_gen_id(synth_id, sizeof(synth_id));
   int rc = db2_artifact_write(synth_id, "synthesis", "committed", scope_kind, scope_id, "curator",
                               0.0, payload_json ? payload_json : "{}");
   free(payload_json);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "kb.curator.synth", "failed to write synthesis for '%s'", topic_name);
      cJSON_Delete(cites);
      return 0;
   }

   /* Link the synthesis `about` the topic entity (suppresses re-pick) and
    * `cites` each source it drew from. */
   db2_artifact_link(synth_id, ent_id, "about");
   cJSON *c = NULL;
   cJSON_ArrayForEach(c, cites)
   {
      if (cJSON_IsString(c) && c->valuestring[0])
         db2_artifact_link(synth_id, c->valuestring, "cites");
   }
   cJSON_Delete(cites);

   aimee_log(LOG_INFO, "kb.curator.synth", "synthesized topic '%s' from %d source(s) -> %s",
             topic_name, n_sources, synth_id);
   return 1;
}
