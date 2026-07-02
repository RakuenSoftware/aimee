/* server_mcp.c: handle mcp.call -- dispatches MCP tool calls within the server */
#include "server.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "dstr.h"
#include "commands.h"
#include "db2/curiosity.h"
#include "memory.h"
#include "index.h"
#include "code_span.h"
#include "db1.h"
#include "kb_client.h"
#include "dashboard.h"
#include "work_queue.h"
#include "mcp_tools.h"
#include "mcp_git.h"
#include "git_verify.h"
#include "workspace_turn.h"
#include "notes.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "agent_pipeline.h"
#include "delegate_economics.h"
#include "delegate_patch_coordinator.h"
#include "platform_path.h"
#include "lsp.h"
#include "server_mcp_learning.h"
#include "server_mcp_process.h"
#include "server_mcp_skill.h"
#include "server_mcp_delegate.h"
#include "server_mcp_workflows.h"
#include "wfe_advance_exec.h"  /* advance_request interactive-driver executor (S2) */
#include "wfe_block_resolve.h" /* per-block externalization guard (S2 sub-slice 4) */
#include "server_mcp_gateway.h"
#include "server_http.h"
#include "server_pipeline.h" /* handle_pipeline_* for the pipeline.* MCP tools */
#include "headers/conversation_context.h"
#include "headers/payload_rewrite.h"
#include "headers/session_search_tool.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>
#include "agent_help_data.h"
int agent_load_config(agent_config_t *cfg);
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* printf-append into buf at pos, returning the new pos clamped to [0, cap].
 * snprintf returns the would-be length, so the raw `pos += snprintf(...)` idiom
 * lets pos run past cap; a later `sizeof(buf) - pos` then wraps to a huge size_t
 * and the next write lands out of bounds. These MCP response builders loop over
 * query results (e.g. up to 20 memory_t each with content[2048]) into an
 * 8 KiB buf, so a result set with long content overflows it — clamp instead. */
static int mcp_appendf(char *buf, int pos, int cap, const char *fmt, ...)
{
   if (pos < 0)
      return 0;
   if (pos >= cap)
      return cap;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + pos, (size_t)(cap - pos), fmt, ap);
   va_end(ap);
   if (n < 0)
      return pos;
   pos += n;
   return pos > cap ? cap : pos;
}
static cJSON *text_content(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static cJSON *json_result_content(cJSON *result)
{
   char *rendered = cJSON_PrintUnformatted(result);
   cJSON_Delete(result);
   cJSON *content = rendered ? text_content(rendered) : text_content("{}");
   free(rendered);
   return content;
}
static int send_mcp_result(server_conn_t *conn, cJSON *content)
{
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "content", content);
   return server_send_ok(conn, resp);
}
static int send_mcp_result_structured(server_conn_t *conn, cJSON *content, cJSON *structured)
{
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "content", content);
   cJSON_AddItemToObject(resp, "structuredContent", structured);
   return server_send_ok(conn, resp);
}

static int handle_mcp_ensemble_review(server_conn_t *conn, cJSON *args)
{
   cJSON *diff = cJSON_GetObjectItemCaseSensitive(args, "diff");
   if (!cJSON_IsString(diff) || !diff->valuestring || !diff->valuestring[0])
      return server_send_error(conn, "ensemble_review requires 'diff'", NULL);
   if (strlen(diff->valuestring) < 20)
      return server_send_error(conn, "ensemble_review requires 'diff' of at least 20 characters",
                               NULL);

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return server_send_error(conn, "out of memory", NULL);
   cJSON_AddStringToObject(body, "prompt", diff->valuestring);
   cJSON_AddStringToObject(body, "mode", "review");
   cJSON *brief = cJSON_GetObjectItemCaseSensitive(args, "brief");
   if (brief)
   {
      if (!cJSON_IsObject(brief) && !cJSON_IsString(brief))
      {
         cJSON_Delete(body);
         return server_send_error(conn, "ensemble_review 'brief' must be a string or object", NULL);
      }
      cJSON *brief_copy = cJSON_Duplicate(brief, 1);
      if (!brief_copy)
      {
         cJSON_Delete(body);
         return server_send_error(conn, "out of memory", NULL);
      }
      cJSON_AddItemToObject(body, "brief", brief_copy);
   }
   cJSON *rounds = cJSON_GetObjectItemCaseSensitive(args, "rounds");
   if (rounds)
   {
      if (!cJSON_IsNumber(rounds) || rounds->valuedouble < 1 || rounds->valuedouble > 16 ||
          rounds->valuedouble != (double)rounds->valueint)
      {
         cJSON_Delete(body);
         return server_send_error(conn, "ensemble_review 'rounds' must be an integer from 1 to 16",
                                  NULL);
      }
      cJSON_AddNumberToObject(body, "rounds", rounds->valuedouble);
   }
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(args, "turns");
   if (turns)
   {
      if (!cJSON_IsString(turns) || (strcmp(turns->valuestring, "parallel") != 0 &&
                                     strcmp(turns->valuestring, "sequential") != 0))
      {
         cJSON_Delete(body);
         return server_send_error(conn, "ensemble_review 'turns' must be parallel or sequential",
                                  NULL);
      }
      cJSON_AddStringToObject(body, "turns", turns->valuestring);
   }

   char *line = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!line)
      return server_send_error(conn, "out of memory", NULL);

   char respbuf[8192];
   int st = server_http_submit_op_run("delegate.roundtable", line, conn->capabilities, respbuf,
                                      sizeof(respbuf));
   free(line);
   if (st < 200 || st >= 300)
   {
      cJSON *err = cJSON_Parse(respbuf);
      cJSON *msg = err ? cJSON_GetObjectItemCaseSensitive(err, "error") : NULL;
      const char *text = cJSON_IsString(msg) ? msg->valuestring : "could not queue ensemble_review";
      int rc = server_send_error(conn, text, NULL);
      cJSON_Delete(err);
      return rc;
   }

   cJSON *snap = cJSON_Parse(respbuf);
   if (!snap)
      return server_send_error(conn, "could not parse queued run", NULL);
   cJSON *id = cJSON_GetObjectItemCaseSensitive(snap, "id");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(snap, "status");
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "run_id", cJSON_IsString(id) ? id->valuestring : "");
   cJSON_AddStringToObject(resp, "id", cJSON_IsString(id) ? id->valuestring : "");
   cJSON_AddStringToObject(resp, "object", "op.run");
   cJSON_AddStringToObject(resp, "method", "delegate.roundtable");
   cJSON_AddStringToObject(resp, "status", cJSON_IsString(status) ? status->valuestring : "queued");
   cJSON_Delete(snap);
   return server_send_ok(conn, resp);
}
static cJSON *tool_get_help(cJSON *args)
{
   cJSON *jtopic = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "topic") : NULL;
   const char *topic =
       (cJSON_IsString(jtopic) && jtopic->valuestring[0]) ? jtopic->valuestring : NULL;

   char *doc = malloc(agent_help_data_len + 1);
   if (!doc)
      return text_content("error: out of memory");
   memcpy(doc, agent_help_data, agent_help_data_len);
   doc[agent_help_data_len] = '\0';

   if (!topic)
   {
      /* Return compact topic index */
      size_t cap = agent_help_data_len + 128;
      char *index = malloc(cap);
      if (!index)
      {
         free(doc);
         return text_content("error: out of memory");
      }
      size_t pos = 0;
      int n = snprintf(index + pos, cap - pos,
                       "Aimee delegate reference. Call get_help(topic) for details.\n\nTopics:\n");
      if (n < 0)
         n = 0;
      pos += (size_t)n < cap - pos ? (size_t)n : cap - pos - 1;
      char *p = doc;
      while (*p)
      {
         if (p[0] == '#' && p[1] == '#' && p[2] == ' ')
         {
            char *nl = strchr(p + 3, '\n');
            int len = nl ? (int)(nl - p - 3) : (int)strlen(p + 3);
            n = snprintf(index + pos, cap - pos, "  %.*s\n", len, p + 3);
            if (n < 0)
               n = 0;
            if ((size_t)n >= cap - pos)
            {
               pos = cap - 1;
               break;
            }
            pos += (size_t)n;
         }
         char *nl = strchr(p, '\n');
         p = nl ? nl + 1 : p + strlen(p);
      }
      index[pos] = '\0';
      free(doc);
      cJSON *content = text_content(index);
      free(index);
      return content;
   }

   /* Normalize topic: lowercase, map - and _ to space */
   char norm_topic[128];
   int tlen = (int)strlen(topic);
   if (tlen >= (int)sizeof(norm_topic))
      tlen = (int)sizeof(norm_topic) - 1;
   for (int i = 0; i < tlen; i++)
   {
      char c = (char)tolower((unsigned char)topic[i]);
      norm_topic[i] = (c == '-' || c == '_') ? ' ' : c;
   }
   norm_topic[tlen] = '\0';

   /* Scan for matching ## section */
   char *section_start = NULL;
   char *p = doc;
   while (*p)
   {
      char *nl = strchr(p, '\n');
      int linelen = nl ? (int)(nl - p) : (int)strlen(p);

      if (linelen > 3 && p[0] == '#' && p[1] == '#' && p[2] == ' ')
      {
         if (section_start)
         {
            /* Start of next section: null-terminate here and return */
            *p = '\0';
            break;
         }
         int slen = linelen - 3;
         char norm_sec[128];
         if (slen >= (int)sizeof(norm_sec))
            slen = (int)sizeof(norm_sec) - 1;
         for (int i = 0; i < slen; i++)
         {
            char c = (char)tolower((unsigned char)p[3 + i]);
            norm_sec[i] = (c == '-' || c == '_') ? ' ' : c;
         }
         norm_sec[slen] = '\0';
         if (strstr(norm_sec, norm_topic) || strstr(norm_topic, norm_sec))
            section_start = p;
      }
      p = nl ? nl + 1 : p + linelen;
   }

   if (!section_start)
   {
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg),
               "unknown topic '%s'. Call get_help() with no args for topic list.", topic);
      free(doc);
      return text_content(errmsg);
   }

   cJSON *result = text_content(section_start);
   free(doc);
   return result;
}

/* Extract scope_type + scope_value from a canonical filter object.
 * Priority: workspace > project > session > user. */
static void parse_filter_scope(cJSON *filter, const char **scope_type, const char **scope_value)
{
   *scope_type = NULL;
   *scope_value = NULL;
   if (!cJSON_IsObject(filter))
      return;
   cJSON *scope = cJSON_GetObjectItemCaseSensitive(filter, "scope");
   if (!cJSON_IsObject(scope))
      return;
   static const char *keys[] = {"workspace", "project", "session", "user", NULL};
   for (int i = 0; keys[i]; i++)
   {
      cJSON *jv = cJSON_GetObjectItemCaseSensitive(scope, keys[i]);
      if (cJSON_IsString(jv) && jv->valuestring[0] && strcmp(jv->valuestring, "any") != 0 &&
          strcmp(jv->valuestring, "current") != 0)
      {
         *scope_type = keys[i];
         *scope_value = jv->valuestring;
         return;
      }
   }
}

static cJSON *tool_search_memory(cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   if (!cJSON_IsString(jq))
      return text_content("error: missing 'query' parameter");

   const char *scope_type = NULL;
   const char *scope_value = NULL;
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(args, "filter");
   parse_filter_scope(jf, &scope_type, &scope_value);

   memory_t facts[20];
   /* Graph-code fusion is always on for recall. */
   int count;
   if (scope_type && scope_type[0])
      count = kb_client_memory_find_facts_scoped_ex(jq->valuestring, scope_type, scope_value, 20,
                                                    facts, 20, "on");
   else
      count = kb_client_memory_find_facts_ex(jq->valuestring, 20, facts, 20, "on");
   if (count < 0)
      return text_content("error: knowledge service search index unavailable; server-side "
                          "maintenance is required");

   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "No facts found for '%s'", jq->valuestring);
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Found %d fact(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- **%s** [%s/%s]: %s\n", facts[i].key,
                           facts[i].tier, facts[i].kind, facts[i].content);
   }
   return text_content(buf);
}

static cJSON *tool_memory_mutate(cJSON *args)
{
   cJSON *jv = cJSON_GetObjectItemCaseSensitive(args, "verb");
   if (!cJSON_IsString(jv))
      return text_content("error: missing 'verb' parameter");
   const char *verb = jv->valuestring;

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "id");
   cJSON *jky = cJSON_GetObjectItemCaseSensitive(args, "key");
   cJSON *jct = cJSON_GetObjectItemCaseSensitive(args, "content");
   cJSON *jti = cJSON_GetObjectItemCaseSensitive(args, "tier");
   cJSON *jkn = cJSON_GetObjectItemCaseSensitive(args, "kind");
   cJSON *jcf = cJSON_GetObjectItemCaseSensitive(args, "confidence");
   cJSON *jre = cJSON_GetObjectItemCaseSensitive(args, "reason");

   int64_t id = cJSON_IsNumber(jid) ? (int64_t)jid->valuedouble : 0;
   const char *key = cJSON_IsString(jky) ? jky->valuestring : NULL;
   const char *content = cJSON_IsString(jct) ? jct->valuestring : NULL;
   const char *tier = (cJSON_IsString(jti) && jti->valuestring[0]) ? jti->valuestring : "L2";
   const char *kind = (cJSON_IsString(jkn) && jkn->valuestring[0]) ? jkn->valuestring : "fact";
   double confidence = cJSON_IsNumber(jcf) ? jcf->valuedouble : 1.0;
   const char *reason = cJSON_IsString(jre) ? jre->valuestring : NULL;

   char buf[256];
   if (strcmp(verb, "store") == 0)
   {
      if (!key || !content)
         return text_content("error: store requires 'key' and 'content'");
      memory_t out;
      memset(&out, 0, sizeof(out));
      int rc = kb_client_memory_insert(tier, kind, key, content, confidence, NULL, &out);
      if (rc != 0)
         return text_content("error: store failed");
      snprintf(buf, sizeof(buf), "stored memory id=%lld key=%s", (long long)out.id, key);
   }
   else if (strcmp(verb, "update") == 0)
   {
      if (id <= 0 || !content)
         return text_content("error: update requires 'id' and 'content'");
      if (kb_client_memory_update(id, content) != 0)
         return text_content("error: update failed");
      snprintf(buf, sizeof(buf), "updated memory id=%lld", (long long)id);
   }
   else if (strcmp(verb, "supersede") == 0)
   {
      if (id <= 0 || !content)
         return text_content("error: supersede requires 'id' and 'content'");
      memory_t out;
      memset(&out, 0, sizeof(out));
      if (kb_client_memory_supersede(id, content, confidence, NULL, &out) != 0)
         return text_content("error: supersede failed");
      snprintf(buf, sizeof(buf), "superseded id=%lld new id=%lld", (long long)id,
               (long long)out.id);
   }
   else if (strcmp(verb, "forget") == 0)
   {
      if (id <= 0)
         return text_content("error: forget requires 'id'");
      if (kb_client_memory_delete(id) != 0)
         return text_content("error: forget failed");
      snprintf(buf, sizeof(buf), "forgot memory id=%lld", (long long)id);
   }
   else if (strcmp(verb, "affirm") == 0)
   {
      if (id <= 0)
         return text_content("error: affirm requires 'id'");
      if (kb_client_memory_touch(id) != 0)
         return text_content("error: affirm failed");
      snprintf(buf, sizeof(buf), "affirmed memory id=%lld", (long long)id);
   }
   else if (strcmp(verb, "reject") == 0)
   {
      if (id <= 0)
         return text_content("error: reject requires 'id'");
      if (kb_client_memory_reject(id, reason) != 0)
         return text_content("error: reject failed");
      snprintf(buf, sizeof(buf), "rejected memory id=%lld", (long long)id);
   }
   else
   {
      snprintf(buf, sizeof(buf), "error: unknown verb '%s'", verb);
   }
   return text_content(buf);
}

static cJSON *tool_memory_ask(cJSON *args, cJSON **structured_out)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (!cJSON_IsString(jq))
      return text_content("error: missing 'query' parameter");

   memory_answer_result_t result;
   memset(&result, 0, sizeof(result));
   int limit = cJSON_IsNumber(jl) ? jl->valueint : 5;
   if (kb_client_memory_ask(jq->valuestring, NULL, NULL, limit, &result) != 0)
      return text_content(result.error[0] ? result.error : "memory_ask failed");

   cJSON *structured = cJSON_CreateObject();
   if (!structured)
      return text_content("error: out of memory");
   cJSON_AddStringToObject(structured, "query", jq->valuestring);
   cJSON_AddStringToObject(structured, "answer", result.answer);
   cJSON_AddNumberToObject(structured, "confidence", result.confidence);
   cJSON_AddStringToObject(structured, "evidence_mode", result.evidence_mode);
   cJSON_AddBoolToObject(structured, "no_answer", result.no_answer);
   cJSON_AddBoolToObject(structured, "low_confidence", result.low_confidence);
   cJSON *trace = cJSON_AddObjectToObject(structured, "evidence_trace");
   if (trace)
   {
      cJSON_AddStringToObject(trace, "decision",
                              memory_answer_evidence_decision_str(&result.evidence));
      cJSON_AddStringToObject(trace, "reason", memory_answer_evidence_reason_str(&result.evidence));
      cJSON *ids = cJSON_AddArrayToObject(trace, "candidate_ids");
      for (int i = 0; ids && i < result.evidence.candidate_id_count; i++)
         cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)result.evidence.candidate_ids[i]));
      cJSON_AddNumberToObject(trace, "ranked_count", result.evidence.ranked_count);
      cJSON_AddNumberToObject(trace, "anchor_id", (double)result.evidence.anchor_id);
      cJSON_AddNumberToObject(trace, "anchor_rank", result.evidence.anchor_rank);
      cJSON_AddNumberToObject(trace, "topk_grounding", result.evidence.topk_grounding);
      cJSON_AddNumberToObject(trace, "anchor_coverage", result.evidence.anchor_coverage);
      cJSON_AddNumberToObject(trace, "cluster_coverage", result.evidence.cluster_coverage);
      cJSON_AddNumberToObject(trace, "threshold", result.evidence.threshold);
      cJSON_AddNumberToObject(trace, "chunk_floor", result.evidence.chunk_floor);
      cJSON_AddBoolToObject(trace, "structural", result.evidence.structural);
      cJSON_AddBoolToObject(trace, "exempt", result.evidence.exempt);
      cJSON_AddBoolToObject(trace, "trace_truncated", result.evidence.trace_truncated);
   }
   cJSON *citations = cJSON_AddArrayToObject(structured, "citations");
   for (int i = 0; i < result.citation_count; i++)
   {
      cJSON *citation = cJSON_CreateObject();
      cJSON_AddNumberToObject(citation, "memory_id", (double)result.citation_ids[i]);
      cJSON_AddItemToArray(citations, citation);
   }
   *structured_out = structured;

   char summary[2048];
   if (result.no_answer)
      snprintf(summary, sizeof(summary), "No confident answer for \"%s\"", jq->valuestring);
   else
      snprintf(summary, sizeof(summary), "%s", result.answer);
   return text_content(summary);
}

static cJSON *tool_search_graph(cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (!cJSON_IsString(jq))
      return text_content("error: missing 'query' parameter");

   int limit = cJSON_IsNumber(jl) ? jl->valueint : 10;
   memory_relation_t rels[20];
   int count = kb_client_memory_search_graph(jq->valuestring, limit, rels, 20);
   if (count < 0)
      return text_content("error: knowledge service unavailable; the memory store is unreachable "
                          "(server-side maintenance is required)");

   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "No graph relations found for '%s'",
                        jq->valuestring);
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Found %d graph relation(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- %s [%s] %s (%s)\n", rels[i].src_entity,
                           rels[i].relation, rels[i].dst_entity,
                           rels[i].valid_at[0] ? rels[i].valid_at : "undated");
   }
   return text_content(buf);
}

static cJSON *tool_get_episode(cJSON *args)
{
   cJSON *jk = cJSON_GetObjectItemCaseSensitive(args, "episode_key");
   if (!cJSON_IsString(jk))
      return text_content("error: missing 'episode_key' parameter");

   memory_episode_t episode;
   if (kb_client_memory_get_episode(jk->valuestring, &episode) != 0)
      return text_content("No episode found.");

   char buf[4096];
   snprintf(buf, sizeof(buf), "Episode: %s\nSession: %s\nTime: %s\nMemory ID: %lld\n\n%s",
            episode.episode_key, episode.source_session,
            episode.reference_time[0] ? episode.reference_time : "unknown",
            (long long)episode.memory_id, episode.episode_text);
   return text_content(buf);
}

static cJSON *tool_get_entity(cJSON *args)
{
   cJSON *je = cJSON_GetObjectItemCaseSensitive(args, "entity");
   if (!cJSON_IsString(je))
      return text_content("error: missing 'entity' parameter");

   memory_entity_profile_t profile;
   if (kb_client_memory_get_entity_profile(je->valuestring, &profile) != 0)
      return text_content("No entity profile found.");

   char buf[4096];
   int pos = 0;
   pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Entity: %s\nMentions: %d\nRelations: %d\n",
                     profile.entity, profile.mention_count, profile.relation_count);
   if (profile.latest_episode[0])
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Latest episode: %s\n", profile.latest_episode);
   if (profile.summary[0])
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Summary: %s\n", profile.summary);
   return text_content(buf);
}

static cJSON *tool_get_entity_edges(cJSON *args)
{
   cJSON *je = cJSON_GetObjectItemCaseSensitive(args, "entity");
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (!cJSON_IsString(je))
      return text_content("error: missing 'entity' parameter");

   int limit = cJSON_IsNumber(jl) ? jl->valueint : 10;
   memory_relation_t rels[20];
   int count = kb_client_memory_get_entity_edges(je->valuestring, limit, rels, 20);
   if (count < 0)
      return text_content("error: knowledge service unavailable; the memory store is unreachable "
                          "(server-side maintenance is required)");

   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos +=
          snprintf(buf + pos, sizeof(buf) - pos, "No edges found for entity '%s'", je->valuestring);
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Edges for %s:\n\n", je->valuestring);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- %s [%s] %s\n", rels[i].src_entity,
                           rels[i].relation, rels[i].dst_entity);
   }
   return text_content(buf);
}

static cJSON *tool_get_context_block(cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   cJSON *jb = cJSON_GetObjectItemCaseSensitive(args, "block_type");
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (!cJSON_IsString(jq))
      return text_content("error: missing 'query' parameter");

   const char *block_type = cJSON_IsString(jb) ? jb->valuestring : "general";
   int limit = cJSON_IsNumber(jl) ? jl->valueint : 5;
   char *ctx = kb_client_memory_context_block(jq->valuestring, block_type, limit);
   if (!ctx)
      return text_content("No context block available.");
   cJSON *result = text_content(ctx);
   free(ctx);
   return result;
}

static cJSON *tool_memory_get(cJSON *args)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "id");
   cJSON *jh = cJSON_GetObjectItemCaseSensitive(args, "handle");
   int64_t id = 0;
   if (cJSON_IsNumber(jid))
      id = (int64_t)jid->valuedouble;
   else if (cJSON_IsString(jh))
   {
      const char *s = jh->valuestring;
      if (strncmp(s, "memory:", 7) == 0)
         s += 7;
      id = (int64_t)strtoll(s, NULL, 10);
   }
   if (id <= 0)
      return text_content("error: missing memory id or memory:<id> handle");

   memory_t m;
   if (kb_client_memory_get(id, &m) != 0)
      return text_content("No memory found.");

   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "Memory: memory:%lld\nTier: %s\nKind: %s\nKey: %s\nConfidence: %.3f\n",
                (long long)m.id, m.tier, m.kind, m.key, m.confidence);
   if (m.headline[0])
      dstr_appendf(&d, "Headline: %s\n", m.headline);
   if (m.updated_at[0])
      dstr_appendf(&d, "Updated: %s\n", m.updated_at);
   dstr_append_str(&d, "\n");
   dstr_append_str(&d, m.content);
   char *rendered = dstr_steal(&d);
   cJSON *result = text_content(rendered ? rendered : "");
   free(rendered);
   return result;
}

static cJSON *tool_list_facts(void)
{
   memory_t facts[64];
   int count = kb_client_memory_list(TIER_L2, KIND_FACT, 64, facts, 64);
   if (count < 0)
      return text_content("error: knowledge service unavailable; the memory store is unreachable "
                          "(server-side maintenance is required)");

   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "No L2 facts stored.");
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "%d fact(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- **%s**: %s\n", facts[i].key,
                           facts[i].content);
   }
   return text_content(buf);
}

static cJSON *tool_memory_briefing(cJSON *args)
{
   int limit_tokens = MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS;
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(args, "limit_tokens");
   if (cJSON_IsNumber(jlimit))
      limit_tokens = (int)jlimit->valuedouble;

   cJSON *bundle = kb_client_memory_briefing(limit_tokens);
   if (!bundle)
      return text_content("error: memory_briefing failed");

   char *rendered = cJSON_PrintUnformatted(bundle);
   cJSON_Delete(bundle);
   if (!rendered)
      return text_content("error: could not render briefing");

   cJSON *content = text_content(rendered);
   free(rendered);
   return content;
}

static cJSON *tool_get_identity(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   if (config_load(&cfg) != 0)
      return text_content("error: could not load config");

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return text_content("error: out of memory");
   cJSON_AddItemToObject(obj, "charter", identity_charter_json(&cfg));
   cJSON_AddItemToObject(obj, "local_operator", identity_local_operator_json());
   cJSON_AddItemToObject(obj, "working_profile", identity_working_profile_json());

   char *rendered = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!rendered)
      return text_content("error: could not render identity");
   cJSON *content = text_content(rendered);
   free(rendered);
   return content;
}

static cJSON *tool_list_curiosity_items(cJSON *args)
{
   cJSON *js = cJSON_GetObjectItemCaseSensitive(args, "state");
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   const char *state = CURIOSITY_STATE_OPEN;
   if (cJSON_IsString(js))
      state = js->valuestring[0] ? js->valuestring : NULL;
   int limit = cJSON_IsNumber(jl) ? jl->valueint : 20;
   if (limit <= 0)
      limit = 20;
   if (limit > 128)
      limit = 128;

   /* Shared knowledge lives behind the knowledge service; aimee-server reaches it via RPC.
    * The response already has the {"status":"ok","items":[...]} shape
    * the agent expects, so we forward it as the tool result. */
   char *json = kb_client_curiosity_list_json(state, limit);
   if (!json)
      return text_content("error: knowledge service unavailable for curiosity list");
   cJSON *content = text_content(json);
   free(json);
   return content;
}

static cJSON *tool_create_prospective_memory(cJSON *args)
{
   cJSON *jt = cJSON_GetObjectItemCaseSensitive(args, "trigger_text");
   cJSON *ja = cJSON_GetObjectItemCaseSensitive(args, "action_text");
   if (!cJSON_IsString(jt) || !jt->valuestring[0])
      return text_content("error: missing 'trigger_text'");
   if (!cJSON_IsString(ja) || !ja->valuestring[0])
      return text_content("error: missing 'action_text'");

   const char *ae = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "anchor_entity"))
                        ? cJSON_GetObjectItemCaseSensitive(args, "anchor_entity")->valuestring
                        : "";
   const char *af = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "anchor_file"))
                        ? cJSON_GetObjectItemCaseSensitive(args, "anchor_file")->valuestring
                        : "";
   const char *re = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "recurrence"))
                        ? cJSON_GetObjectItemCaseSensitive(args, "recurrence")->valuestring
                        : NULL;
   const char *vu = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "valid_until"))
                        ? cJSON_GetObjectItemCaseSensitive(args, "valid_until")->valuestring
                        : "";

   char *envelope =
       kb_client_memory_prospective_create_json(jt->valuestring, ja->valuestring, ae, af, re, vu);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = resp ? cJSON_GetObjectItemCaseSensitive(resp, "message") : NULL;
      const char *err = cJSON_IsString(msg)
                            ? msg->valuestring
                            : "create_prospective_memory failed (check recurrence value)";
      char buf[256];
      snprintf(buf, sizeof(buf), "error: %s", err);
      cJSON_Delete(resp);
      return text_content(buf);
   }
   cJSON *prospective = cJSON_GetObjectItemCaseSensitive(resp, "prospective");
   char *rendered = NULL;
   if (cJSON_IsObject(prospective))
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, prospective);
      rendered = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   if (!rendered)
      return text_content("error: render failed");
   cJSON *content = text_content(rendered);
   free(rendered);
   return content;
}

static cJSON *tool_list_prospective_memories(cJSON *args)
{
   const char *state = NULL;
   cJSON *jst = cJSON_GetObjectItemCaseSensitive(args, "state");
   if (cJSON_IsString(jst) && jst->valuestring[0])
      state = jst->valuestring;
   int limit = 50;
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (cJSON_IsNumber(jl))
      limit = (int)jl->valuedouble;
   if (limit < 1)
      limit = 1;
   if (limit > 256)
      limit = 256;

   char *envelope = kb_client_memory_prospective_list_json(state, limit);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *prospectives = resp ? cJSON_GetObjectItemCaseSensitive(resp, "prospectives") : NULL;
   char *rendered = NULL;
   if (cJSON_IsArray(prospectives))
   {
      cJSON *detached = cJSON_DetachItemViaPointer(resp, prospectives);
      rendered = detached ? cJSON_PrintUnformatted(detached) : NULL;
      cJSON_Delete(detached);
   }
   cJSON_Delete(resp);
   cJSON *content = text_content(rendered ? rendered : "[]");
   free(rendered);
   return content;
}

static cJSON *tool_complete_prospective_memory(cJSON *args)
{
   cJSON *ji = cJSON_GetObjectItemCaseSensitive(args, "id");
   if (!cJSON_IsNumber(ji))
      return text_content("error: missing 'id'");
   int64_t id = (int64_t)ji->valuedouble;
   char *envelope = kb_client_memory_prospective_complete_json(id);
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   int ok = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
   cJSON_Delete(resp);
   if (!ok)
      return text_content("error: could not complete reminder (already terminal or missing)");
   char buf[128];
   snprintf(buf, sizeof(buf), "{\"id\":%lld,\"state\":\"completed\"}", (long long)id);
   return text_content(buf);
}

static cJSON *tool_get_host(cJSON *args)
{
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(args, "name");
   if (!cJSON_IsString(jn))
      return text_content("error: missing 'name' parameter");

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || !cfg.network.ssh_entry[0])
      return text_content("error: no network configuration found");

   for (int i = 0; i < cfg.network.host_count; i++)
   {
      agent_net_host_t *h = &cfg.network.hosts[i];
      if (strcasecmp(h->name, jn->valuestring) == 0)
      {
         char buf[512];
         if (h->port > 0)
            snprintf(buf, sizeof(buf), "Host: %s\nIP: %s\nPort: %d\nUser: %s\nDescription: %s",
                     h->name, h->ip, h->port, h->user, h->desc);
         else
            snprintf(buf, sizeof(buf), "Host: %s\nIP: %s\nUser: %s\nDescription: %s", h->name,
                     h->ip, h->user, h->desc);
         return text_content(buf);
      }
   }

   char buf[256];
   snprintf(buf, sizeof(buf), "Host '%s' not found. Use list_hosts to see all available hosts.",
            jn->valuestring);
   return text_content(buf);
}

static cJSON *tool_list_hosts(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || !cfg.network.ssh_entry[0])
      return text_content("No network configuration found.");

   char buf[16384];
   int pos = 0;
   agent_network_t *nw = &cfg.network;

   pos = mcp_appendf(buf, pos, (int)sizeof(buf), "SSH Entry: %s\n\n", nw->ssh_entry);

   if (nw->network_count > 0)
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Networks:\n");
      for (int i = 0; i < nw->network_count; i++)
         pos = mcp_appendf(buf, pos, (int)sizeof(buf), "  %-16s %-20s %s\n", nw->networks[i].name,
                           nw->networks[i].cidr, nw->networks[i].desc);
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "\n");
   }

   if (nw->host_count > 0)
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Hosts (%d):\n", nw->host_count);
      for (int i = 0; i < nw->host_count && pos < (int)sizeof(buf) - 256; i++)
      {
         agent_net_host_t *h = &nw->hosts[i];
         if (h->port > 0)
            pos = mcp_appendf(buf, pos, (int)sizeof(buf), "  %-20s %s:%d  %-8s %s\n", h->name,
                              h->ip, h->port, h->user, h->desc);
         else
            pos = mcp_appendf(buf, pos, (int)sizeof(buf), "  %-20s %-20s %-8s %s\n", h->name, h->ip,
                              h->user, h->desc);
      }
   }

   return text_content(buf);
}

#include "server_mcp_ast_grep.inc"

static cJSON *tool_find_symbol(cJSON *args)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "identifier");
   if (!cJSON_IsString(jid))
      return text_content("error: missing 'identifier' parameter");

   term_hit_t hits[20];
   int count = kb_client_index_find(jid->valuestring, hits, 20);

   char buf[4096];
   int pos = 0;
   if (count == 0)
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "No symbol found for '%s'", jid->valuestring);
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Found %d match(es) for '%s':\n\n", count,
                        jid->valuestring);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++)
      {
         /* Show the body span (line-line_end) when known, so a `file::symbol`
          * read can fetch exactly that range; fall back to the start line. */
         if (hits[i].line_end > hits[i].line)
            pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- %s:%d-%d [%s] in project '%s'\n",
                              hits[i].file_path, hits[i].line, hits[i].line_end, hits[i].kind,
                              hits[i].project);
         else
            pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- %s:%d [%s] in project '%s'\n",
                              hits[i].file_path, hits[i].line, hits[i].kind, hits[i].project);
      }
   }
   return text_content(buf);
}

static cJSON *tool_preview_blast_radius(cJSON *args)
{
   cJSON *jproj = cJSON_GetObjectItemCaseSensitive(args, "project");
   cJSON *jpaths = cJSON_GetObjectItemCaseSensitive(args, "paths");
   if (!cJSON_IsString(jproj) || !cJSON_IsArray(jpaths))
      return text_content("error: missing 'project' or 'paths' parameter");

   int cnt = cJSON_GetArraySize(jpaths);
   if (cnt < 1 || cnt > 100)
      return text_content("error: paths must contain 1-100 entries");

   char *paths[100];
   for (int i = 0; i < cnt; i++)
   {
      cJSON *item = cJSON_GetArrayItem(jpaths, i);
      paths[i] = cJSON_IsString(item) ? item->valuestring : "";
   }

   char *json = kb_client_index_blast_radius_preview_json(jproj->valuestring, paths, cnt);
   cJSON *content = text_content(
       json ? json : "{\"status\":\"error\",\"message\":\"knowledge service unavailable\"}");
   free(json);
   return content;
}

static cJSON *tool_record_attempt(cJSON *args)
{
   cJSON *jap = cJSON_GetObjectItemCaseSensitive(args, "approach");
   cJSON *joc = cJSON_GetObjectItemCaseSensitive(args, "outcome");
   if (!cJSON_IsString(jap) || !cJSON_IsString(joc))
      return text_content("error: missing 'approach' or 'outcome' parameter");

   cJSON *jtc = cJSON_GetObjectItemCaseSensitive(args, "task_context");
   cJSON *jls = cJSON_GetObjectItemCaseSensitive(args, "lesson");

   cJSON *val = cJSON_CreateObject();
   cJSON_AddStringToObject(val, "task_context", cJSON_IsString(jtc) ? jtc->valuestring : "");
   cJSON_AddStringToObject(val, "approach", jap->valuestring);
   cJSON_AddStringToObject(val, "outcome", joc->valuestring);
   cJSON_AddStringToObject(val, "lesson", cJSON_IsString(jls) ? jls->valuestring : "");

   char *json_val = cJSON_PrintUnformatted(val);
   cJSON_Delete(val);
   if (!json_val)
      return text_content("error: failed to serialize attempt");

   char key[64];
   static int attempt_counter = 0;
   snprintf(key, sizeof(key), "attempt:%d", ++attempt_counter);

   const char *sid = session_id();
   int rc = db1_wm_set(sid, key, json_val, "attempt", 14400);
   free(json_val);

   char buf[128];
   if (rc == 0)
      snprintf(buf, sizeof(buf), "Recorded attempt as %s", key);
   else
      snprintf(buf, sizeof(buf), "error: failed to store attempt");
   return text_content(buf);
}

static cJSON *tool_list_attempts(cJSON *args)
{
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(args, "filter");
   const char *filter = cJSON_IsString(jf) ? jf->valuestring : NULL;

   const char *sid = session_id();
   wm_entry_t entries[WM_MAX_RESULTS];
   int count = db1_wm_list(sid, "attempt", entries, WM_MAX_RESULTS);

   char buf[8192];
   int pos = 0;

   if (count == 0)
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "No attempts recorded this session.");
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "Previous attempts (%d total):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++)
      {
         cJSON *v = cJSON_Parse(entries[i].value);
         if (!v)
            continue;

         cJSON *jtc = cJSON_GetObjectItemCaseSensitive(v, "task_context");
         cJSON *jap = cJSON_GetObjectItemCaseSensitive(v, "approach");
         cJSON *joc = cJSON_GetObjectItemCaseSensitive(v, "outcome");
         cJSON *jls = cJSON_GetObjectItemCaseSensitive(v, "lesson");

         const char *tc = cJSON_IsString(jtc) ? jtc->valuestring : "";
         const char *ap = cJSON_IsString(jap) ? jap->valuestring : "";
         const char *oc = cJSON_IsString(joc) ? joc->valuestring : "";
         const char *ls = cJSON_IsString(jls) ? jls->valuestring : "";

         if (filter && filter[0] && !strstr(tc, filter) && !strstr(ap, filter))
         {
            cJSON_Delete(v);
            continue;
         }

         pos +=
             snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                      "- Context: %s\n  Tried: %s\n  Result: %s\n  Lesson: %s\n\n", tc, ap, oc, ls);
         cJSON_Delete(v);
      }
   }

   return text_content(buf);
}

/* --- Workflow tool handler --- */

static cJSON *tool_store_workflow(cJSON *args)
{
   cJSON *jr = cJSON_GetObjectItemCaseSensitive(args, "rule");
   cJSON *jsig = cJSON_GetObjectItemCaseSensitive(args, "signal_type");
   if (!cJSON_IsString(jr) || !jr->valuestring[0] || !cJSON_IsString(jsig) || !jsig->valuestring[0])
      return text_content("error: missing 'rule' and/or 'signal_type' parameter");

   /* Project: prefer explicit arg; fall back to cwd-derived workspace label. */
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "project");
   char workspace[128] = "";
   if (cJSON_IsString(jp) && jp->valuestring[0])
      snprintf(workspace, sizeof(workspace), "%s", jp->valuestring);
   else
   {
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)))
      {
         config_t cfg;
         if (config_load(&cfg) == 0)
         {
            for (int i = 0; i < cfg.workspace_count; i++)
            {
               size_t wlen = strlen(cfg.workspaces[i]);
               if (wlen == 0)
                  continue;
               if (strncmp(cwd, cfg.workspaces[i], wlen) == 0 &&
                   (cwd[wlen] == '/' || cwd[wlen] == '\0'))
               {
                  const char *slash = strrchr(cfg.workspaces[i], '/');
                  const char *name = slash ? slash + 1 : cfg.workspaces[i];
                  snprintf(workspace, sizeof(workspace), "%s", name);
                  break;
               }
            }
         }
      }
   }

   if (!workspace[0])
      return text_content("error: no workspace determined from cwd; pass 'project' explicitly");

   /* User-explicit store: high confidence. */
   int64_t id = kb_client_memory_upsert_workflow(workspace, jsig->valuestring, jr->valuestring, 1.0,
                                                 session_id());
   if (id <= 0)
      return text_content("error: failed to store workflow memory");

   char buf[256];
   snprintf(buf, sizeof(buf), "Stored workflow:%s:%s (memory id %lld)", workspace,
            jsig->valuestring, (long long)id);
   return text_content(buf);
}

/* --- Note tool handlers --- */

static cJSON *tool_create_note(cJSON *args)
{
   cJSON *jt = cJSON_GetObjectItemCaseSensitive(args, "title");
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(args, "content");
   if (!cJSON_IsString(jt) || !cJSON_IsString(jc))
      return text_content("error: missing 'title' and/or 'content' parameters");

   cJSON *jtg = cJSON_GetObjectItemCaseSensitive(args, "tags");
   const char *tags = cJSON_IsString(jtg) ? jtg->valuestring : NULL;

   /* Shared knowledge lives behind the knowledge service; aimee-server reaches it via RPC. */
   char *json = kb_client_note_create_json(jt->valuestring, jc->valuestring, tags, session_id());
   if (!json)
      return text_content("error: knowledge service unavailable for note create");

   /* Decode the response and render the human-readable summary the
    * existing tool contract returned. */
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return text_content("error: invalid response from knowledge service");

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      char err[256];
      snprintf(err, sizeof(err), "error: failed to create note: %s",
               cJSON_IsString(msg) ? msg->valuestring : "unknown");
      cJSON_Delete(resp);
      return text_content(err);
   }
   cJSON *note = cJSON_GetObjectItemCaseSensitive(resp, "note");
   cJSON *title = note ? cJSON_GetObjectItemCaseSensitive(note, "title") : NULL;
   cJSON *slug = note ? cJSON_GetObjectItemCaseSensitive(note, "slug") : NULL;
   cJSON *id = note ? cJSON_GetObjectItemCaseSensitive(note, "id") : NULL;
   char buf[512];
   snprintf(buf, sizeof(buf), "Note saved: **%s** (slug: %s, id: %lld)",
            cJSON_IsString(title) ? title->valuestring : "",
            cJSON_IsString(slug) ? slug->valuestring : "",
            cJSON_IsNumber(id) ? (long long)id->valuedouble : 0LL);
   cJSON_Delete(resp);
   return text_content(buf);
}

static cJSON *render_notes_response(const char *json, const char *empty_msg, int include_content)
{
   if (!json)
      return text_content("error: knowledge service unavailable for notes");
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
      return text_content("error: invalid response from knowledge service");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      char err[256];
      snprintf(err, sizeof(err), "error: %s",
               cJSON_IsString(msg) ? msg->valuestring : "notes lookup failed");
      cJSON_Delete(resp);
      return text_content(err);
   }
   cJSON *notes = cJSON_GetObjectItemCaseSensitive(resp, "notes");
   int count = cJSON_IsArray(notes) ? cJSON_GetArraySize(notes) : 0;
   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos = mcp_appendf(buf, pos, (int)sizeof(buf), "%s", empty_msg);
   else
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf),
                        include_content ? "Found %d note(s):\n\n" : "Investigation notes (%d):\n\n",
                        count);
      cJSON *n = NULL;
      cJSON_ArrayForEach(n, notes)
      {
         if (pos >= (int)sizeof(buf) - 1024)
            break;
         cJSON *t = cJSON_GetObjectItemCaseSensitive(n, "title");
         cJSON *tg = cJSON_GetObjectItemCaseSensitive(n, "tags");
         cJSON *u = cJSON_GetObjectItemCaseSensitive(n, "updated_at");
         cJSON *c = cJSON_GetObjectItemCaseSensitive(n, "content");
         const char *title = cJSON_IsString(t) ? t->valuestring : "";
         const char *tags = cJSON_IsString(tg) ? tg->valuestring : "";
         const char *updated = cJSON_IsString(u) ? u->valuestring : "";
         if (include_content)
         {
            const char *content = cJSON_IsString(c) ? c->valuestring : "";
            pos = mcp_appendf(buf, pos, (int)sizeof(buf),
                              "### %s\n*Tags: %s | Updated: %s*\n\n%s\n\n---\n\n", title, tags,
                              updated, content);
         }
         else
            pos = mcp_appendf(buf, pos, (int)sizeof(buf), "- **%s** [%s] (updated: %s)\n", title,
                              tags, updated);
      }
   }
   cJSON_Delete(resp);
   return text_content(buf);
}

static cJSON *tool_list_notes(cJSON *args)
{
   cJSON *jtg = cJSON_GetObjectItemCaseSensitive(args, "tag");
   cJSON *jlm = cJSON_GetObjectItemCaseSensitive(args, "limit");
   const char *tag = cJSON_IsString(jtg) ? jtg->valuestring : NULL;
   int limit = cJSON_IsNumber(jlm) ? jlm->valueint : 20;

   char *json = kb_client_note_list_json(tag, limit);
   cJSON *out = render_notes_response(json, "No investigation notes found.", 0);
   free(json);
   return out;
}

static cJSON *tool_search_notes(cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   if (!cJSON_IsString(jq))
      return text_content("error: missing 'query' parameter");

   char *json = kb_client_note_search_json(jq->valuestring, 10);
   char empty[256];
   snprintf(empty, sizeof(empty), "No notes matching '%s'.", jq->valuestring);
   cJSON *out = render_notes_response(json, empty, 1);
   free(json);
   return out;
}

/* --- Coordinated job tools --- */

static cJSON *tool_job_start(cJSON *args)
{
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "plan_id");
   if (!cJSON_IsNumber(jp))
      return text_content("error: missing 'plan_id' parameter");

   int plan_id = jp->valueint;
   cJSON *jmc = cJSON_GetObjectItemCaseSensitive(args, "max_concurrent");
   int max_concurrent = cJSON_IsNumber(jmc) ? jmc->valueint : DB1_COORD_DEFAULT_PAR;

   /* Load plan steps */
   plan_t plan;
   if (db1_execution_plan_get(plan_id, &plan) != 0)
      return text_content("error: plan not found");

   int job_id = db1_coord_job_create(plan_id, max_concurrent);
   if (job_id < 0)
      return text_content("error: failed to create job");

   int added = 0;
   for (int i = 0; i < plan.step_count; i++)
   {
      if (db1_coord_job_add_task(job_id, plan.steps[i].id, "[]", "", "", "") > 0)
         added++;
   }

   char buf[256];
   snprintf(buf, sizeof(buf), "Created job #%d from plan #%d: %d tasks, max %d concurrent", job_id,
            plan_id, added, max_concurrent);
   return text_content(buf);
}

static cJSON *tool_job_status(cJSON *args)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "job_id");
   if (!cJSON_IsNumber(jid))
      return text_content("error: missing 'job_id' parameter");
   int job_id = jid->valueint;
   db1_coord_job_t job;
   if (db1_coord_job_get(job_id, &job) != 0)
      return text_content("error: job not found");
   db1_coord_task_t tasks[DB1_COORD_MAX_TASKS];
   int count = db1_coord_job_list_tasks(job_id, tasks, DB1_COORD_MAX_TASKS);
   agent_config_t agent_cfg;
   agent_config_t *agent_cfg_ptr = NULL;
   memset(&agent_cfg, 0, sizeof(agent_cfg));
   if (agent_load_config(&agent_cfg) == 0)
      agent_cfg_ptr = &agent_cfg;
   delegate_economics_report_t econ;
   delegate_economics_build_report(&job, tasks, count, agent_cfg_ptr, &econ);
   delegate_patch_report_t patches;
   delegate_patch_coordinator_build_report(&job, tasks, count, &patches);
   char buf[8192];
   int pos = 0;
   pos = mcp_appendf(buf, pos, (int)sizeof(buf),
                     "Job #%d (plan #%d): **%s**\n\n"
                     "| Metric | Value |\n|---|---|\n"
                     "| Max concurrent | %d |\n| Total tasks | %d |\n| Done | %d |\n"
                     "| Running | %d |\n| Failed | %d |\n| Pending | %d |\n",
                     job.id, job.plan_id, job.status, job.max_concurrent, job.total_tasks,
                     job.done_tasks, job.running_tasks, job.failed_tasks,
                     job.total_tasks - job.done_tasks - job.failed_tasks - job.running_tasks);
   pos += snprintf(
       buf + pos, sizeof(buf) - (size_t)pos,
       "\n### Delegation report\n\n"
       "| Metric | Value |\n|---|---|\n"
       "| Cost model | %s |\n| Delegates | %d total, %d tier-0, %d tier-1, %d tier-2, %d tier-3, "
       "%d unknown |\n"
       "| Delegate tokens estimated | %d%s |\n| Supervisor prompt tokens estimated | %d |\n"
       "| Focused tests run by delegates | %d/%d |\n| Structured handoffs valid | %d/%d |\n"
       "| Invalid handoffs | %d |\n| Manual integration events | %d |\n"
       "| Reviewer blocking findings | %d |\n| Supervisor work remaining | %d decisions |\n"
       "| Verdict | %s |\n| Recommendation | %s |\n",
       delegate_economics_cost_model_label(), econ.delegate_count, econ.tier_counts[0],
       econ.tier_counts[1], econ.tier_counts[2], econ.tier_counts[3], econ.unknown_tier_count,
       econ.delegate_tokens_estimated, econ.tokenized_delegate_results == 0 ? " (unavailable)" : "",
       econ.supervisor_prompt_tokens_estimated, econ.delegates_with_focused_tests,
       econ.delegate_count, econ.valid_handoffs, econ.handoff_count, econ.invalid_handoffs,
       econ.manual_integration_events, econ.reviewer_findings_blocking,
       econ.supervisor_actions_required, delegate_economics_verdict_text(econ.verdict),
       econ.recommendation);
   char patch_brief[1024];
   pos = mcp_appendf(buf, pos, (int)sizeof(buf), "\n### Patch coordinator\n\n```text\n%s\n```\n",
                     delegate_patch_coordinator_brief(&patches, patch_brief, sizeof(patch_brief)));
   if (count > 0)
   {
      pos = mcp_appendf(buf, pos, (int)sizeof(buf),
                        "\n### Tasks\n\n| ID | Status | Patch | Delegate | Step | Files |\n"
                        "|---|---|---|---|---|---|\n");
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++)
      {
         const char *patch = "-";
         if (i < patches.task_count && patches.tasks[i].patch_state[0])
            patch = patches.tasks[i].patch_state;
         pos = mcp_appendf(buf, pos, (int)sizeof(buf), "| %d | %s | %s | %s | %d | %s |\n",
                           tasks[i].id, tasks[i].status, patch,
                           tasks[i].claimed_by[0] ? tasks[i].claimed_by : "-", tasks[i].step_id,
                           (tasks[i].files[0] && strcmp(tasks[i].files, "[]") != 0) ? tasks[i].files
                                                                                    : "-");
      }
   }
   return text_content(buf);
}

static void conn_active_verify(server_conn_t *conn, const char *sid, int set)
{
   if (!conn || !sid || !sid[0])
      return;
   pthread_mutex_lock(&conn->mutex);
   if (set && !conn->closing)
      snprintf(conn->active_verify_session, sizeof(conn->active_verify_session), "%s", sid);
   else if (!set && strcmp(conn->active_verify_session, sid) == 0)
      conn->active_verify_session[0] = '\0';
   pthread_mutex_unlock(&conn->mutex);
}

/* The git MCP tools, in one table: handler + whether the op mutates the tree
 * (which gates it on a worktree/HEAD mismatch). Single source of truth — the
 * dispatch and the mutating-guard below both read it, so adding a git tool is
 * one row. git_verify is dispatched separately (it takes ctx + verify_sid). */
typedef cJSON *(*git_tool_fn)(cJSON *args);
static const struct
{
   const char *name;
   git_tool_fn fn;
   int mutating;
} git_tool_table[] = {
    {"git_status", handle_git_status, 0}, {"git_commit", handle_git_commit, 1},
    {"git_push", handle_git_push, 1},     {"git_branch", handle_git_branch, 0},
    {"git_log", handle_git_log, 0},       {"git_diff_summary", handle_git_diff_summary, 0},
    {"git_pr", handle_git_pr, 1},         {"git_pull", handle_git_pull, 1},
    {"git_clone", handle_git_clone, 0},   {"git_stash", handle_git_stash, 0},
    {"git_tag", handle_git_tag, 0},       {"git_fetch", handle_git_fetch, 0},
    {"git_reset", handle_git_reset, 1},   {"git_restore", handle_git_restore, 1},
    {"git_issue", handle_git_issue, 0},
};

static cJSON *dispatch_git_tool(server_ctx_t *ctx, server_conn_t *conn, const char *tool,
                                cJSON *args, const char *sid)
{
   char verify_sid_buf[SERVER_SESSION_ID_MAX] = "";
   const char *verify_sid = sid;
   int is_verify = strcmp(tool, "git_verify") == 0;
   if (is_verify && (!verify_sid || !verify_sid[0]))
      snprintf(verify_sid_buf, sizeof(verify_sid_buf), "conn-%ld-%p",
               conn ? (long)conn->peer_pid : 0L, (void *)conn);
   if (verify_sid_buf[0])
      verify_sid = verify_sid_buf;
   if (is_verify)
      conn_active_verify(conn, verify_sid, 1);
   if (sid && sid[0])
      session_id_set_override(sid);

   /* A `mirror`-workspace cwd points at the CLIENT's path, which does not exist
    * server-side; remap it to the reconstructed server-side worktree (driving the
    * mirror lifecycle: ensure + reconstruct) so the git tool — e.g. `/pr` from the
    * gateway — operates on the real tree. No-op for shared/detached workspaces. */
   {
      cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
      if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
      {
         char work_cwd[MAX_PATH_LEN];
         if (workspace_turn_resolve_mirror_cwd(jcwd->valuestring, work_cwd, sizeof(work_cwd)))
            cJSON_ReplaceItemInObject(args, "cwd", cJSON_CreateString(work_cwd));
      }
   }

   /* A `detached`-workspace cwd ALSO points at the CLIENT's path (the workspace
    * lives on the serving client), so there is nothing to chdir into locally.
    * Bind the detached provider for this cwd's workspace — exactly as the
    * chat-turn boundary does — so the git tool's rev-parse / exec marshal over
    * the runner channel to the serving client, which holds the real tree (and
    * its own creds). No-op for shared/mirror/unregistered cwds (returns 0). */
   int detached_bound = 0;
   {
      cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
      if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
         detached_bound = workspace_turn_bind_active(jcwd->valuestring);
   }

   char *mismatch_err = NULL;
   int resolved = mcp_chdir_git_root(NULL, 0, args, &mismatch_err);

   if (resolved < 0)
   {
      run_cmd_set_cwd(NULL);
      if (detached_bound)
         workspace_turn_unbind_active();
      if (sid && sid[0])
         session_id_clear_override();
      if (is_verify)
         conn_active_verify(conn, verify_sid, 0);
      return text_content("error: session worktree is unavailable (chdir failed). Refusing to run "
                          "git operation on the main repository.");
   }
   /* Resolve the handler + mutating-ness from the one git-tool table. git_verify
    * is not in the table (different handler signature); it is dispatched below. */
   git_tool_fn git_fn = NULL;
   int is_mutating = 0;
   for (size_t i = 0; i < sizeof(git_tool_table) / sizeof(git_tool_table[0]); i++)
      if (strcmp(tool, git_tool_table[i].name) == 0)
      {
         git_fn = git_tool_table[i].fn;
         is_mutating = git_tool_table[i].mutating;
         break;
      }

   if (mismatch_err && is_mutating)
   {
      run_cmd_set_cwd(NULL);
      if (detached_bound)
         workspace_turn_unbind_active();
      if (sid && sid[0])
         session_id_clear_override();
      cJSON *err_resp = text_content(mismatch_err);
      free(mismatch_err);
      return err_resp;
   }
   cJSON *content = NULL;

   if (strcmp(tool, "git_verify") == 0)
      content = handle_git_verify(ctx, args, verify_sid);
   else if (git_fn)
      content = git_fn(args);

   run_cmd_set_cwd(NULL);
   if (detached_bound)
      workspace_turn_unbind_active();
   if (sid && sid[0])
      session_id_clear_override();
   if (is_verify)
      conn_active_verify(conn, verify_sid, 0);
   if (mismatch_err && content)
   {
      cJSON *item = cJSON_GetArrayItem(content, 0);
      if (item)
      {
         cJSON *text = cJSON_GetObjectItem(item, "text");
         if (text && cJSON_IsString(text))
         {
            char *new_text = malloc(strlen(mismatch_err) + strlen(text->valuestring) + 16);
            if (new_text)
            {
               sprintf(new_text, "%s\n\n%s", mismatch_err, text->valuestring);
               cJSON_ReplaceItemInObject(item, "text", cJSON_CreateString(new_text));
               free(new_text);
            }
         }
      }
      free(mismatch_err);
   }
   else if (mismatch_err)
   {
      free(mismatch_err);
   }

   return content;
}

#include "server_mcp_call_table.inc"

/* ── Discovery meta-tools (P2) ────────────────────────────────────────────────
 * find_tools / describe_tool introspect the FULL served catalog (unfiltered by
 * the presentation profile) so a lean tools/list loses no reach: the model can
 * discover any tool's name + schema and then call it by name. Read-only; they
 * return MCP `content` like any other content-producing tool. */
static int mcp_ci_contains(const char *haystack, const char *needle)
{
   if (!needle || !needle[0])
      return 1; /* empty query matches everything */
   if (!haystack)
      return 0;
   size_t nlen = strlen(needle);
   for (const char *h = haystack; *h; h++)
      if (strncasecmp(h, needle, nlen) == 0)
         return 1;
   return 0;
}

static cJSON *mcp_tool_find_tools(cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   const char *q = cJSON_IsString(jq) ? jq->valuestring : NULL;
   int limit = 50;
   cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
   if (cJSON_IsNumber(jl) && jl->valueint > 0)
      limit = jl->valueint;

   cJSON *all = mcp_build_full_served_list();
   cJSON *result = cJSON_CreateObject();
   cJSON *matches = cJSON_AddArrayToObject(result, "tools");
   int shown = 0, total = 0;
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, all)
   {
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
      cJSON *ds = cJSON_GetObjectItemCaseSensitive(t, "description");
      const char *name = cJSON_IsString(nm) ? nm->valuestring : "";
      const char *desc = cJSON_IsString(ds) ? ds->valuestring : "";
      if (!mcp_ci_contains(name, q) && !mcp_ci_contains(desc, q))
         continue;
      total++;
      if (shown >= limit)
         continue;
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "name", name);
      cJSON_AddStringToObject(m, "description", desc);
      cJSON_AddItemToArray(matches, m);
      shown++;
   }
   cJSON_AddNumberToObject(result, "count", shown);
   cJSON_AddNumberToObject(result, "total", total);
   if (total > shown)
      cJSON_AddBoolToObject(result, "truncated", 1);
   cJSON_AddStringToObject(
       result, "hint",
       "Call describe_tool(name) for a tool's input schema, then call the tool by name.");
   cJSON_Delete(all);
   return json_result_content(result);
}

static cJSON *mcp_tool_describe_tool(cJSON *args)
{
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(args, "name");
   if (!cJSON_IsString(jn) || !jn->valuestring[0])
      return text_content("error: describe_tool requires a 'name'");
   const char *want = jn->valuestring;

   cJSON *all = mcp_build_full_served_list();
   cJSON *found = NULL;
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, all)
   {
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
      if (cJSON_IsString(nm) && strcmp(nm->valuestring, want) == 0)
      {
         found = cJSON_Duplicate(t, 1);
         break;
      }
   }
   cJSON_Delete(all);
   if (!found)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "error: unknown tool '%s' (use find_tools to discover names)",
               want);
      return text_content(msg);
   }
   return json_result_content(found);
}

int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jtool = cJSON_GetObjectItemCaseSensitive(req, "tool");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(req, "arguments");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   const char *sid = (jsid && cJSON_IsString(jsid)) ? jsid->valuestring : NULL;

   if (!cJSON_IsString(jtool))
      return server_send_error(conn, "missing 'tool' parameter", NULL);

   int owns_jargs = 0;
   if (!jargs)
   {
      jargs = cJSON_CreateObject();
      if (!jargs)
         return server_send_error(conn, "out of memory", NULL);
      owns_jargs = 1;
   }
   if (cJSON_IsString(jcwd) && jcwd->valuestring[0] && cJSON_IsObject(jargs) &&
       !cJSON_GetObjectItemCaseSensitive(jargs, "cwd"))
      cJSON_AddStringToObject(jargs, "cwd", jcwd->valuestring);

   const char *tool = jtool->valuestring;
   cJSON *content = NULL;
   cJSON *structured = NULL;

   /* Family multiplex (P4): if `tool` is a collapsed family (pipeline/diagnose/
    * session/lsp/note/…), rewrite it to the legacy <family>_<command> name so all
    * downstream routing + capability gating runs unchanged. */
   char fam_tool[96];
   {
      int fd = mcp_family_demux(tool, jargs, fam_tool, sizeof(fam_tool));
      if (fd < 0)
      {
         char emsg[160];
         snprintf(emsg, sizeof(emsg), "%s requires a valid 'command' (see describe_tool)", tool);
         if (owns_jargs)
            cJSON_Delete(jargs);
         return server_send_error(conn, emsg, NULL);
      }
      if (fd == 1)
         tool = fam_tool;
   }

   /* S2 sub-slice 4: pre-delivery externalization guard at the tool-DISPATCH seam.
    * A bound enforced run whose gate.deliver has not passed may not call an
    * externalization primitive (pr.open/merge, push, egress, ...) regardless of tool
    * visibility -- this is the narrow run-state invariant behind the ingress surface
    * strip ("tool visibility alone is not a security boundary"). Default-OFF (dial
    * unset -> ALLOW); soft warns + allows; hard refuses. The decision is audited
    * inside the guard. Runs before every dispatch branch so it also covers the
    * workflow-tool and git_ externalization paths. */
   if (wfe_mcp_toolcall_action(sid, tool) == WFE_TC_DENY)
   {
      if (owns_jargs)
         cJSON_Delete(jargs);
      return server_send_error(
          conn, "refused: this action externalizes work before the review/delivery gate has passed",
          NULL);
   }

   if (strcmp(tool, "delegate") == 0)
   {
      int rc = handle_mcp_delegate_call(ctx, conn, jargs, sid);
      if (owns_jargs)
         cJSON_Delete(jargs);
      return rc;
   }

   if (strcmp(tool, "delegate_status") == 0)
   {
      int rc = handle_delegate_status(ctx, conn, jargs);
      if (owns_jargs)
         cJSON_Delete(jargs);
      return rc;
   }

   if (strcmp(tool, "ensemble_review") == 0)
   {
      int rc = handle_mcp_ensemble_review(conn, jargs);
      if (owns_jargs)
         cJSON_Delete(jargs);
      return rc;
   }

   /* Delegate reply: forward to existing handler */
   if (strcmp(tool, "delegate_reply") == 0)
   {
      cJSON *dreq = cJSON_CreateObject();
      cJSON_AddStringToObject(dreq, "method", "delegate.reply");
      cJSON *jid = cJSON_GetObjectItemCaseSensitive(jargs, "delegation_id");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(jargs, "content");
      if (cJSON_IsString(jid))
         cJSON_AddStringToObject(dreq, "delegation_id", jid->valuestring);
      if (cJSON_IsString(jc))
         cJSON_AddStringToObject(dreq, "content", jc->valuestring);
      int rc = handle_delegate_reply(ctx, conn, dreq);
      cJSON_Delete(dreq);
      if (owns_jargs)
         cJSON_Delete(jargs);
      return rc;
   }

   /* Roundtable authoring pipeline tools (first-class MCP citizens): route each
    * pipeline_* tool to its handler, which sends its own response on conn (like
    * ensemble_review/delegate). Capability is enforced against the matching
    * pipeline.* method (status/list = read; others = delegate; gate also needs an
    * operator principal inside the handler). MCP arg names mirror the method's. */
   if (strncmp(tool, "pipeline_", 9) == 0)
   {
      static const struct
      {
         const char *tool;
         const char *method;
         int (*fn)(server_ctx_t *, server_conn_t *, cJSON *);
      } pipeline_tools[] = {
          {"pipeline_start", "pipeline.start", handle_pipeline_start},
          {"pipeline_status", "pipeline.status", handle_pipeline_status},
          {"pipeline_list", "pipeline.list", handle_pipeline_list},
          {"pipeline_cancel", "pipeline.cancel", handle_pipeline_cancel},
          {"pipeline_resume", "pipeline.resume", handle_pipeline_resume},
          {"pipeline_advance", "pipeline.advance", handle_pipeline_advance},
          {"pipeline_gate", "pipeline.gate", handle_pipeline_gate},
      };
      for (size_t i = 0; i < sizeof(pipeline_tools) / sizeof(pipeline_tools[0]); i++)
      {
         if (strcmp(tool, pipeline_tools[i].tool) != 0)
            continue;
         uint32_t required = server_capability_for_method(pipeline_tools[i].method);
         if (required && conn && (conn->capabilities & required) == 0)
         {
            if (owns_jargs)
               cJSON_Delete(jargs);
            return server_send_error(conn, "forbidden: insufficient capabilities", NULL);
         }
         int rc = pipeline_tools[i].fn(ctx, conn, jargs);
         if (owns_jargs)
            cJSON_Delete(jargs);
         return rc;
      }
   }

   /* Discovery meta-tools (P2): pure introspection of the full catalog; set
    * content and fall through to the normal send path. */
   if (strcmp(tool, "find_tools") == 0)
      content = mcp_tool_find_tools(jargs);
   else if (strcmp(tool, "describe_tool") == 0)
      content = mcp_tool_describe_tool(jargs);

   struct mcp_call call = {ctx, conn, jargs, sid, tool, &structured};
   mcp_tool_handler_fn handler = content ? NULL : mcp_tool_lookup(tool);
   if (handler)
   {
      content = handler(&call);
   }
   else if (server_mcp_is_workflow_tool(tool))
   {
      int rc = server_mcp_handle_workflow_tool(conn, tool, jargs, &content, &structured);
      if (rc != 0)
      {
         if (owns_jargs)
            cJSON_Delete(jargs);
         return rc;
      }
   }
   else if (strncmp(tool, "git_", 4) == 0)
   {
      content = dispatch_git_tool(ctx, conn, tool, jargs, sid);
   }
   else if (strcmp(tool, "git") == 0)
   {
      /* Single multiplexed git tool: `command` selects the subcommand, which maps
       * to the existing git_<command> dispatch (the git_* names stay callable). */
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(jargs, "command");
      if (!cJSON_IsString(jc) || !jc->valuestring[0])
      {
         if (owns_jargs)
            cJSON_Delete(jargs);
         return server_send_error(conn, "git requires a 'command' (status, commit, branch, pr, …)",
                                  NULL);
      }
      char gtool[64];
      snprintf(gtool, sizeof(gtool), "git_%s", jc->valuestring);
      content = dispatch_git_tool(ctx, conn, gtool, jargs, sid);
   }
   if (!content)
      content = mcp_gateway_tool_dispatch(tool, jargs);
   if (!content)
      content = server_mcp_process_tool(tool, jargs);
   if (!content)
   {
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg), "unknown MCP tool: %s", tool);
      if (owns_jargs)
         cJSON_Delete(jargs);
      return server_send_error(conn, errmsg, NULL);
   }

   int rc;
   if (structured)
      rc = send_mcp_result_structured(conn, content, structured);
   else
      rc = send_mcp_result(conn, content);
   if (owns_jargs)
      cJSON_Delete(jargs);
   return rc;
}
