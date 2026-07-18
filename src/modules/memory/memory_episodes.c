/* memory_episodes.c: per-session episode cards and scene clustering.
 * Extracted from memory_advanced.c. */
#include "aimee.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/entity_edges.h"
#include "db2/memory_query.h"
#include "db2/memory_relations.h"
#include "db2/memory_scenes.h"
#endif
#include "log.h"
#include "memory.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Per-Session Episode Cards --- */

/*
 * memory_episode_card_parse: parse LLM JSON into a memory_episode_card_t.
 *
 * Expected schema (all fields optional except title):
 * {
 *   "session_id": "...",
 *   "title": "Camping trip with family",
 *   "participants": ["Caroline", "Melanie"],
 *   "places": ["Yosemite"],
 *   "events": ["arrived May 1", "hiked Half Dome May 2"],
 *   "outcomes": ["everyone safe", "lost a tent pole"],
 *   "open_threads": ["Caroline mentioned wanting to return in fall"]
 * }
 */
int memory_episode_card_parse(const char *json, memory_episode_card_t *out)
{
   if (!json || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *j = cJSON_Parse(json);
   if (!j)
      return -1;

   cJSON *title = cJSON_GetObjectItemCaseSensitive(j, "title");
   if (!cJSON_IsString(title) || !title->valuestring[0])
   {
      cJSON_Delete(j);
      return -1;
   }
   snprintf(out->title, sizeof(out->title), "%s", title->valuestring);

   cJSON *sid = cJSON_GetObjectItemCaseSensitive(j, "session_id");
   if (cJSON_IsString(sid) && sid->valuestring[0])
      snprintf(out->session_id, sizeof(out->session_id), "%s", sid->valuestring);

#define JOIN_ARRAY(field, key, sep)                                                                \
   do                                                                                              \
   {                                                                                               \
      cJSON *arr__ = cJSON_GetObjectItemCaseSensitive(j, key);                                     \
      if (cJSON_IsArray(arr__))                                                                    \
      {                                                                                            \
         size_t pos__ = 0;                                                                         \
         const char *sep__ = (sep);                                                                \
         size_t slen__ = sep__ ? strlen(sep__) : 0;                                                \
         cJSON *el__;                                                                              \
         cJSON_ArrayForEach(el__, arr__)                                                           \
         {                                                                                         \
            if (!cJSON_IsString(el__) || !el__->valuestring[0])                                    \
               continue;                                                                           \
            if (pos__ > 0 && slen__ > 0 && pos__ + slen__ + 1 < sizeof(out->field))                \
            {                                                                                      \
               memcpy(out->field + pos__, sep__, slen__);                                          \
               pos__ += slen__;                                                                    \
            }                                                                                      \
            size_t vlen__ = strlen(el__->valuestring);                                             \
            if (pos__ + vlen__ + 1 >= sizeof(out->field))                                          \
               break;                                                                              \
            memcpy(out->field + pos__, el__->valuestring, vlen__);                                 \
            pos__ += vlen__;                                                                       \
         }                                                                                         \
         out->field[pos__] = '\0';                                                                 \
      }                                                                                            \
   } while (0)

   JOIN_ARRAY(participants, "participants", ", ");
   JOIN_ARRAY(places, "places", ", ");
   JOIN_ARRAY(events, "events", "\n");
   JOIN_ARRAY(outcomes, "outcomes", "\n");
   JOIN_ARRAY(open_threads, "open_threads", "\n");
#undef JOIN_ARRAY

   cJSON_Delete(j);
   return 0;
}

/*
 * memory_episode_card_generate: generate, store, and link an episode card for
 * a closed session.
 *
 * Steps:
 *   1. Collect up to 200 memories for |source_session| (content + id).
 *   2. Build a JSON payload and call cfg->memory_cognify_command.
 *   3. Parse the response as memory_episode_card_t.
 *   4. Store a synthetic memory (KIND_EPISODE) and a memory_unit with
 *      is_episode_card=1.
 *   5. Insert REL_SUMMARISES rows in memory_relations for each constituent
 *      memory.
 * Returns the memory_unit id on success, 0 on failure or if disabled.
 */
int64_t memory_episode_card_generate(const char *source_session, const config_t *cfg)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)source_session;
   (void)cfg;
   return 0;
#else
   if (!source_session || !source_session[0])
      return 0;
   if (!cfg || !cfg->memory_episode_summaries_enabled)
      return 0;
   if (!cfg->memory_cognify_command[0])
      return 0;

   /* Collect session memories */
#define MAX_SESSION_MEMS 200
   db2_memory_id_content_row_t rows[MAX_SESSION_MEMS];
   int mem_count =
       db2_memory_session_id_content_list(source_session, MAX_SESSION_MEMS, rows, MAX_SESSION_MEMS);
   if (mem_count <= 0)
      return 0;
   int64_t mem_ids[MAX_SESSION_MEMS];
   char *mem_texts[MAX_SESSION_MEMS];
   for (int i = 0; i < mem_count; i++)
   {
      mem_ids[i] = rows[i].id;
      mem_texts[i] = strdup(rows[i].content);
   }
#undef MAX_SESSION_MEMS

   /* Build JSON input for the cognifier */
   cJSON *input = cJSON_CreateObject();
   cJSON_AddStringToObject(input, "task", "episode_card");
   cJSON_AddStringToObject(input, "session_id", source_session);
   cJSON *turns = cJSON_AddArrayToObject(input, "turns");
   for (int i = 0; i < mem_count; i++)
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddNumberToObject(t, "id", (double)mem_ids[i]);
      cJSON_AddStringToObject(t, "text", mem_texts[i] ? mem_texts[i] : "");
      cJSON_AddItemToArray(turns, t);
   }
   char *input_str = cJSON_PrintUnformatted(input);
   cJSON_Delete(input);
   for (int i = 0; i < mem_count; i++)
      free(mem_texts[i]);

   if (!input_str)
      return 0;

   char *resp = NULL;
   size_t resp_len = 0;
   int rc = platform_exec_pipe(cfg->memory_cognify_command, input_str, strlen(input_str), &resp,
                               &resp_len);
   free(input_str);
   if (rc != 0 || !resp || resp_len == 0)
   {
      free(resp);
      aimee_log(LOG_WARN, "memory_episode", "cognifier failed for session %s", source_session);
      return 0;
   }

   memory_episode_card_t card;
   rc = memory_episode_card_parse(resp, &card);
   free(resp);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "memory_episode", "episode card parse failed for session %s",
                source_session);
      return 0;
   }

   /* Build card text */
   char card_text[2048];
   snprintf(card_text, sizeof(card_text),
            "Episode: %s\n"
            "Participants: %s\n"
            "Places: %s\n"
            "Events:\n%s\n"
            "Outcomes:\n%s\n"
            "Open threads:\n%s",
            card.title, card.participants[0] ? card.participants : "(none)",
            card.places[0] ? card.places : "(none)", card.events[0] ? card.events : "(none)",
            card.outcomes[0] ? card.outcomes : "(none)",
            card.open_threads[0] ? card.open_threads : "(none)");

   /* Store as a synthetic memory */
   char key[256];
   snprintf(key, sizeof(key), "episode:%s", source_session);
   memory_t m;
   memset(&m, 0, sizeof(m));
   if (memory_insert(TIER_L2, KIND_EPISODE, key, card_text, 1.8, "episode_card", &m) != 0 ||
       m.id <= 0)
      return 0;

   /* Associate the synthetic memory with the session */
   db2_memory_set_source_session(m.id, source_session);

   /* Insert memory_unit with is_episode_card=1 */
   db2_memory_unit_episode_card_insert(m.id, key, card_text);

   /* Fetch the unit id */
   int64_t unit_id = db2_memory_unit_first_episode_card_id(m.id);

   /* Insert REL_SUMMARISES relations */
   if (unit_id > 0)
   {
      char dst_buf[32];
      for (int i = 0; i < mem_count; i++)
      {
         snprintf(dst_buf, sizeof(dst_buf), "memory:%lld", (long long)mem_ids[i]);
         db2_memory_relation_insert(m.id, key, "REL_SUMMARISES", dst_buf, card.title);
      }
   }

   aimee_log(LOG_INFO, "memory_episode", "episode card created for session %s (unit_id=%lld)",
             source_session, (long long)unit_id);
   return unit_id;
#endif
}

/*
 * memory_episode_cards_query: return episode card content strings for a session.
 * Caller must free each returned string.
 */
int memory_episode_cards_query(const char *source_session, char **out, int max)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)source_session;
   (void)out;
   (void)max;
   return 0;
#else
   return db2_memory_episode_cards_query(source_session, out, max);
#endif
}

/* --- Scene Clustering --- */

int memory_cluster_scenes(const char *workspace_id)
{
   (void)workspace_id;
   return 0;
}

int memory_assign_scene(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

/* ========================================================================
 * Typed Knowledge Graph Ontology
 * ======================================================================== */

typedef struct
{
   const char *text;
   memory_relation_kind_t code;
} rel_entry_t;
static const rel_entry_t rel_table[] = {{"depends_on", REL_DEPENDS_ON},
                                        {"implements", REL_IMPLEMENTS},
                                        {"fixes", REL_FIXES},
                                        {"introduced_by", REL_INTRODUCED_BY},
                                        {"tests", REL_TESTS},
                                        {"calls", REL_CALLS},
                                        {"mutates", REL_MUTATES},
                                        {"participated_in", REL_PARTICIPATED_IN},
                                        {"occurred_at", REL_OCCURRED_AT},
                                        {"authored_by", REL_AUTHORED_BY},
                                        {"supersedes", REL_SUPERSEDES},
                                        {"co_edited", REL_CO_EDITED},
                                        {"co_discussed", REL_CO_DISCUSSED},
                                        {"summarises", REL_SUMMARISES},
                                        {NULL, REL_OTHER}};

typedef struct
{
   const char *text;
   memory_node_kind_t code;
} node_entry_t;
static const node_entry_t node_table[] = {{"file", NODE_FILE},
                                          {"function", NODE_FUNCTION},
                                          {"struct", NODE_STRUCT},
                                          {"module", NODE_MODULE},
                                          {"bug", NODE_BUG},
                                          {"commit", NODE_COMMIT},
                                          {"pr", NODE_PR},
                                          {"developer", NODE_DEVELOPER},
                                          {"concept", NODE_CONCEPT},
                                          {"event", NODE_EVENT},
                                          {"person", NODE_PERSON},
                                          {"place", NODE_PLACE},
                                          {"time_expr", NODE_TIME_EXPR},
                                          {"device", NODE_DEVICE},
                                          {"org", NODE_ORG},
                                          {"ip", NODE_IP},
                                          {"scalar", NODE_SCALAR},
                                          {NULL, NODE_OTHER}};

memory_relation_kind_t memory_ontology_relation_from_text(const char *label)
{
   if (!label)
      return REL_OTHER;
   for (int i = 0; rel_table[i].text; i++)
      if (strcasecmp(label, rel_table[i].text) == 0)
         return rel_table[i].code;
   return REL_OTHER;
}

const char *memory_ontology_relation_to_text(memory_relation_kind_t rel)
{
   for (int i = 0; rel_table[i].text; i++)
      if (rel_table[i].code == rel)
         return rel_table[i].text;
   return "other";
}

memory_node_kind_t memory_ontology_node_kind_from_text(const char *label)
{
   if (!label)
      return NODE_OTHER;
   for (int i = 0; node_table[i].text; i++)
      if (strcasecmp(label, node_table[i].text) == 0)
         return node_table[i].code;
   return NODE_OTHER;
}

const char *memory_ontology_node_kind_to_text(memory_node_kind_t kind)
{
   for (int i = 0; node_table[i].text; i++)
      if (node_table[i].code == kind)
         return node_table[i].text;
   return "other";
}

/*
 * Static schema: (subject_kind, relation_id, object_kind) allowed triples.
 * NODE_OTHER = wildcard (any kind allowed for that position).
 */
typedef struct
{
   memory_node_kind_t sk;
   memory_relation_kind_t rel;
   memory_node_kind_t ok;
} schema_rule_t;

static const schema_rule_t schema_rules[] = {{NODE_FILE, REL_CO_EDITED, NODE_FILE},
                                             {NODE_OTHER, REL_CO_DISCUSSED, NODE_OTHER},
                                             {NODE_FUNCTION, REL_DEPENDS_ON, NODE_FUNCTION},
                                             {NODE_MODULE, REL_DEPENDS_ON, NODE_MODULE},
                                             {NODE_FUNCTION, REL_CALLS, NODE_FUNCTION},
                                             {NODE_FUNCTION, REL_MUTATES, NODE_STRUCT},
                                             {NODE_FUNCTION, REL_IMPLEMENTS, NODE_CONCEPT},
                                             {NODE_COMMIT, REL_FIXES, NODE_BUG},
                                             {NODE_COMMIT, REL_INTRODUCED_BY, NODE_DEVELOPER},
                                             {NODE_OTHER, REL_AUTHORED_BY, NODE_DEVELOPER},
                                             {NODE_OTHER, REL_AUTHORED_BY, NODE_PERSON},
                                             {NODE_OTHER, REL_TESTS, NODE_OTHER},
                                             {NODE_PERSON, REL_PARTICIPATED_IN, NODE_EVENT},
                                             {NODE_DEVELOPER, REL_PARTICIPATED_IN, NODE_EVENT},
                                             {NODE_OTHER, REL_OCCURRED_AT, NODE_TIME_EXPR},
                                             {NODE_OTHER, REL_OCCURRED_AT, NODE_PLACE},
                                             {NODE_OTHER, REL_SUPERSEDES, NODE_OTHER},
                                             {NODE_OTHER, REL_SUMMARISES, NODE_OTHER},
                                             /* sentinel */
                                             {NODE_OTHER, REL_OTHER, NODE_OTHER}};

int memory_ontology_validate(memory_node_kind_t sk, memory_relation_kind_t rel,
                             memory_node_kind_t ok)
{
   /* REL_OTHER is experimental — always allowed */
   if (rel == REL_OTHER)
      return 1;

   for (int i = 0; !(schema_rules[i].sk == NODE_OTHER && schema_rules[i].rel == REL_OTHER &&
                     schema_rules[i].ok == NODE_OTHER);
        i++)
   {
      const schema_rule_t *r = &schema_rules[i];
      if (r->rel != rel)
         continue;
      int sk_ok = (r->sk == NODE_OTHER || r->sk == sk);
      int ok_ok = (r->ok == NODE_OTHER || r->ok == ok);
      if (sk_ok && ok_ok)
         return 1;
   }
   return 0;
}

/*
 * memory_graph_walk: multi-hop walk from |seed_entity| through entity_edges,
 * filtering by |relation_mask| (bitmask of memory_relation_kind_t values;
 * RELATION_MASK_ALL to accept every typed relation).
 *
 * Visits up to |max_hops| hops.  For each visited edge, appends an entry to
 * |out| (up to |max| entries).  Returns the number of entries written.
 *
 * When relation_id IS NULL, the row is treated as REL_CO_DISCUSSED and
 * included only when that bit is set in the mask.
 */
int memory_graph_walk(const char *seed_entity, unsigned int relation_mask, int max_hops,
                      graph_walk_entry_t *out, int max)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)seed_entity;
   (void)relation_mask;
   (void)max_hops;
   (void)out;
   (void)max;
   return 0;
#else
   if (!seed_entity || !out || max <= 0)
      return 0;

   /* BFS frontier */
#define WALK_MAX_FRONTIER 256
   char frontier[WALK_MAX_FRONTIER][256];
   int frontier_n = 0;

   /* Visited set to avoid cycles */
   char visited[WALK_MAX_FRONTIER][256];
   int visited_n = 0;

   snprintf(frontier[frontier_n++], 256, "%s", seed_entity);
   snprintf(visited[visited_n++], 256, "%s", seed_entity);

   int count = 0;

   for (int hop = 0; hop < max_hops && frontier_n > 0 && count < max; hop++)
   {
      char next_frontier[WALK_MAX_FRONTIER][256];
      int next_n = 0;

      for (int f = 0; f < frontier_n && count < max; f++)
      {
         const char *node = frontier[f];
         db2_entity_edge_typed_t buf[50];
         int n = db2_entity_edge_walk_step_typed(node, buf, 50);

         for (int b = 0; b < n && count < max; b++)
         {
            int rid = buf[b].relation_id;
            if (rid < 0 || rid >= 100)
               rid = (int)REL_CO_DISCUSSED;
            unsigned bit = (unsigned)rid < 32u ? (1u << (unsigned)rid) : 0u;
            if (!(relation_mask & bit) && relation_mask != RELATION_MASK_ALL)
               continue;

            const char *src = buf[b].source;
            const char *rel = buf[b].relation;
            const char *tgt = buf[b].target;
            if (!src[0] || !rel[0] || !tgt[0])
               continue;

            /* Determine the neighbour (the side that isn't |node|) */
            const char *neighbour = (strcmp(src, node) == 0) ? tgt : src;

            int already = 0;
            for (int v = 0; v < visited_n; v++)
               if (strcmp(visited[v], neighbour) == 0)
               {
                  already = 1;
                  break;
               }
            if (already)
               continue;
            if (visited_n < WALK_MAX_FRONTIER)
               snprintf(visited[visited_n++], 256, "%s", neighbour);

            snprintf(out[count].source, sizeof(out[count].source), "%s", src);
            snprintf(out[count].relation, sizeof(out[count].relation), "%s", rel);
            snprintf(out[count].target, sizeof(out[count].target), "%s", tgt);
            out[count].relation_id = (memory_relation_kind_t)rid;
            out[count].subject_kind = (memory_node_kind_t)buf[b].subject_kind;
            out[count].object_kind = (memory_node_kind_t)buf[b].object_kind;
            out[count].weight = buf[b].weight;
            out[count].hop = hop + 1;
            count++;

            if (next_n < WALK_MAX_FRONTIER)
               snprintf(next_frontier[next_n++], 256, "%s", neighbour);
         }
      }

      frontier_n = next_n < WALK_MAX_FRONTIER ? next_n : WALK_MAX_FRONTIER;
      for (int i = 0; i < frontier_n; i++)
         snprintf(frontier[i], 256, "%s", next_frontier[i]);
   }

   return count;
#undef WALK_MAX_FRONTIER
#endif
}
