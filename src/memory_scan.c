/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* memory_scan.c: JSONL session-log scanner — detect compaction markers,
 * Write/Edit tool calls, decisions; extract and persist memories. Extracted
 * from memory_advanced.c (was merged in as "Memory Scan"). */
#include "aimee.h"
#include "db1_optional.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/entity_edges.h"
#include "db2/entity_profiles.h"
#include "entity_edges.h"
#include "entity_profiles.h"
#endif
#include "memory.h"
#include "memory_ontology.h"
#include "cJSON.h"
#include "log.h"
#include "platform_process.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Memory Scan (from memory_scan.c) --- */
static int extract_file_refs(const char *text, char **out, int max)
{
   int count = 0;
   const char *p = text;

   while (*p && count < max)
   {
      /* Look for / followed by path chars */
      if (*p == '/' &&
          (p == text || isspace((unsigned char)p[-1]) || p[-1] == '"' || p[-1] == '\''))
      {
         const char *start = p;
         p++;
         while (*p && !isspace((unsigned char)*p) && *p != '"' && *p != '\'' && *p != ')' &&
                *p != ']' && *p != '>')
            p++;

         int len = (int)(p - start);
         if (len > 3 && len < MAX_PATH_LEN)
         {
            /* Must contain a dot (file extension) */
            int has_dot = 0;
            for (const char *d = start; d < p; d++)
            {
               if (*d == '.')
               {
                  has_dot = 1;
                  break;
               }
            }
            if (has_dot)
            {
               out[count] = malloc(len + 1);
               if (out[count])
               {
                  memcpy(out[count], start, len);
                  out[count][len] = '\0';
                  count++;
               }
            }
         }
      }
      else
      {
         p++;
      }
   }
   return count;
}

/* --- Detect compaction markers --- */

static int is_compaction_marker(const char *line)
{
   if (strstr(line, "\"type\":\"summary\""))
      return 1;
   if (strstr(line, "[compacted]"))
      return 1;
   return 0;
}

/* --- Detect Write/Edit tool calls for decisions --- */

static int is_tool_decision(const char *line, char *desc, size_t desc_len)
{
   /* Look for tool_use with Write or Edit */
   const char *tool_use = strstr(line, "\"tool_use\"");
   if (!tool_use)
      return 0;

   int is_write = 0;
   const char *name_pos = strstr(line, "\"name\":");
   if (name_pos)
   {
      if (strstr(name_pos, "\"Write\"") || strstr(name_pos, "\"Edit\""))
         is_write = 1;
   }

   if (!is_write)
      return 0;

   /* Extract file_path if present */
   const char *fp_pos = strstr(line, "\"file_path\":");
   if (fp_pos)
   {
      fp_pos += 13; /* skip past "file_path":" */
      while (*fp_pos == ' ' || *fp_pos == '"')
         fp_pos++;
      const char *end = fp_pos;
      while (*end && *end != '"' && *end != ',')
         end++;
      int len = (int)(end - fp_pos);
      if (len > 0 && len < (int)desc_len - 16)
         snprintf(desc, desc_len, "Write/Edit: %.*s", len, fp_pos);
      else
         snprintf(desc, desc_len, "Write/Edit tool call");
   }
   else
   {
      snprintf(desc, desc_len, "Write/Edit tool call");
   }
   return 1;
}

typedef struct
{
   char role[16];
   char *text;
} scan_message_part_t;

static void scan_free_message_parts(scan_message_part_t *parts, int *count)
{
   if (!parts || !count)
      return;
   for (int i = 0; i < *count; i++)
      free(parts[i].text);
   *count = 0;
}

static int scan_append_text(char **buf, size_t *len, size_t *cap, const char *text)
{
   if (!buf || !len || !cap || !text || !text[0])
      return 0;
   size_t add = strlen(text);
   size_t sep = (*len > 0) ? 1 : 0;
   if (*len + sep + add + 1 > *cap)
   {
      size_t next = *cap ? *cap : 128;
      while (*len + sep + add + 1 > next)
         next *= 2;
      char *tmp = realloc(*buf, next);
      if (!tmp)
         return -1;
      *buf = tmp;
      *cap = next;
   }
   if (sep)
      (*buf)[(*len)++] = ' ';
   memcpy(*buf + *len, text, add);
   *len += add;
   (*buf)[*len] = '\0';
   return 0;
}

static void scan_collect_content_text(cJSON *node, char **buf, size_t *len, size_t *cap)
{
   if (!node)
      return;
   if (cJSON_IsString(node))
   {
      (void)scan_append_text(buf, len, cap, node->valuestring);
      return;
   }
   if (cJSON_IsArray(node))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, node) scan_collect_content_text(item, buf, len, cap);
      return;
   }
   if (!cJSON_IsObject(node))
      return;

   cJSON *text = cJSON_GetObjectItemCaseSensitive(node, "text");
   if (!cJSON_IsString(text))
      text = cJSON_GetObjectItemCaseSensitive(node, "output");
   if (!cJSON_IsString(text))
      text = cJSON_GetObjectItemCaseSensitive(node, "result");
   if (cJSON_IsString(text))
      (void)scan_append_text(buf, len, cap, text->valuestring);
}

static const char *scan_message_role(cJSON *msg)
{
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "role"));
   if (role && role[0])
      return role;
   cJSON *author = cJSON_GetObjectItemCaseSensitive(msg, "author");
   role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(author, "role"));
   return (role && role[0]) ? role : "message";
}

static int scan_extract_message_text(const char *line, char *role, size_t role_len, char **text_out)
{
   if (role && role_len > 0)
      role[0] = '\0';
   if (text_out)
      *text_out = NULL;
   if (!line || !text_out)
      return 0;

   cJSON *root = cJSON_Parse(line);
   if (!root)
      return 0;

   cJSON *msg = root;
   cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");

   if (!content)
   {
      cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "message");
      if (!cJSON_IsObject(nested))
         nested = cJSON_GetObjectItemCaseSensitive(root, "item");
      if (cJSON_IsObject(nested))
      {
         msg = nested;
         content = cJSON_GetObjectItemCaseSensitive(msg, "content");
      }
   }

   if (!content)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
      cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "item");
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
      if (cJSON_IsString(method) && strcmp(method->valuestring, "item/completed") == 0 && type &&
          (strcmp(type, "agentMessage") == 0 || strcmp(type, "userMessage") == 0))
      {
         msg = item;
         content = cJSON_GetObjectItemCaseSensitive(item, "text");
         if (role && role_len > 0)
            snprintf(role, role_len, "%s",
                     strcmp(type, "agentMessage") == 0 ? "assistant" : "user");
      }
   }

   if (!content)
   {
      cJSON_Delete(root);
      return 0;
   }

   char *buf = NULL;
   size_t len = 0, cap = 0;
   scan_collect_content_text(content, &buf, &len, &cap);
   if (!buf || !buf[0])
   {
      free(buf);
      cJSON_Delete(root);
      return 0;
   }

   if (role && role_len > 0 && !role[0])
      snprintf(role, role_len, "%s", scan_message_role(msg));
   *text_out = buf;
   cJSON_Delete(root);
   return 1;
}

static int scan_add_message_part(scan_message_part_t *parts, int *count, const char *role,
                                 char *owned_text)
{
   if (!parts || !count || !owned_text)
      return 0;
   if (*count >= 64)
   {
      free(owned_text);
      return 0;
   }
   snprintf(parts[*count].role, sizeof(parts[*count].role), "%s",
            role && role[0] ? role : "message");
   parts[*count].text = owned_text;
   (*count)++;
   return 1;
}

static void scan_build_combined_messages(scan_message_part_t *parts, int count, char *combined,
                                         size_t combined_len)
{
   if (!combined || combined_len == 0)
      return;
   combined[0] = '\0';
   int pos = 0;
   for (int i = 0; i < count; i++)
   {
      if (!parts[i].text || !parts[i].text[0])
         continue;
      int space = (int)combined_len - pos;
      if (space <= 1)
         break;
      int wrote = snprintf(combined + pos, (size_t)space, "%s%s: %s", pos > 0 ? "\n" : "",
                           parts[i].role, parts[i].text);
      if (wrote < 0)
         break;
      if (wrote >= space)
      {
         pos = (int)combined_len - 1;
         break;
      }
      pos += wrote;
   }
}

/* --- Scan a single JSONL file --- */

static int scan_file(const char *session_id, const char *file_path)
{
   /* Get count + max end_line of existing windows for this session */
   int existing_count = 0;
   int max_end_line = 0;
   (void)db1_windows_session_scan_state(session_id, &existing_count, &max_end_line);

   FILE *fp = fopen(file_path, "r");
   if (!fp)
      return -1;

   char *line_buf = malloc(MAX_LINE_LEN);
   if (!line_buf)
   {
      fclose(fp);
      return -1;
   }

   int line_num = 0;
   int seq = existing_count;
   int windows_added = 0;

   /* Accumulate a window of ~20 lines */
   scan_message_part_t message_parts[64];
   memset(message_parts, 0, sizeof(message_parts));
   int message_count = 0;
   char *file_refs[128];
   int file_ref_count = 0;
   char decision_descs[16][256];
   int decision_count = 0;
   int window_start = max_end_line + 1;

   while (fgets(line_buf, MAX_LINE_LEN, fp))
   {
      line_num++;

      /* Skip already-scanned lines */
      if (line_num <= max_end_line)
         continue;

      /* Skip compaction markers */
      if (is_compaction_marker(line_buf))
         continue;

      /* Extract message text with role boundaries instead of flattening the
       * window into one undifferentiated string. */
      {
         char role[16];
         char *text = NULL;
         if (scan_extract_message_text(line_buf, role, sizeof(role), &text))
            (void)scan_add_message_part(message_parts, &message_count, role, text);
      }

      /* Extract file refs from any line */
      {
         char *refs[16];
         int rc = extract_file_refs(line_buf, refs, 16);
         for (int i = 0; i < rc && file_ref_count < 128; i++)
            file_refs[file_ref_count++] = refs[i];
      }

      /* Detect decisions */
      if (decision_count < 16)
      {
         char desc[256];
         if (is_tool_decision(line_buf, desc, sizeof(desc)))
         {
            snprintf(decision_descs[decision_count], 256, "%s", desc);
            decision_count++;
         }
      }

      /* Every ~20 lines or at EOF-ish, flush window */
      if ((line_num - window_start + 1) >= 20 || message_count >= 10)
      {
         /* Build search text and summary from role-marked message boundaries. */
         char combined[4096];
         scan_build_combined_messages(message_parts, message_count, combined, sizeof(combined));

         char *terms[128];
         int term_count = tokenize_for_search(combined, terms, 128);

         /* Build summary (first 200 chars of combined) */
         char summary[1024];
         snprintf(summary, sizeof(summary), "%.200s", combined);

         /* INSERT window */
         char ts[32];
         now_utc(ts, sizeof(ts));
         seq++;

         int64_t window_id = db1_window_create_raw(session_id, seq, summary, ts);

         if (window_id > 0)
         {
            /* INSERT decisions (now owned by DB1). */
            for (int d = 0; d < decision_count; d++)
               (void)db1_decision_record(window_id, decision_descs[d], ts);

            /* INSERT window_terms */
            for (int t = 0; t < term_count; t++)
               (void)db1_window_add_term(window_id, terms[t]);

            /* INSERT window_files */
            for (int f = 0; f < file_ref_count; f++)
               (void)db1_window_add_file(window_id, file_refs[f]);

            (void)db1_window_index_summary(window_id, summary);

            /* Extract entity edges */
            memory_extract_edges(window_id, file_refs, file_ref_count, terms, term_count);

            windows_added++;
         }

         /* Clean up window state */
         scan_free_message_parts(message_parts, &message_count);

         for (int i = 0; i < term_count; i++)
            free(terms[i]);

         for (int i = 0; i < file_ref_count; i++)
            free(file_refs[i]);
         file_ref_count = 0;

         decision_count = 0;
         window_start = line_num + 1;
      }
   }

   /* Flush remaining partial window */
   if (message_count > 0 || file_ref_count > 0)
   {
      char combined[4096];
      scan_build_combined_messages(message_parts, message_count, combined, sizeof(combined));

      char *terms[128];
      int term_count = tokenize_for_search(combined, terms, 128);

      char summary[1024];
      snprintf(summary, sizeof(summary), "%.200s", combined);

      char ts[32];
      now_utc(ts, sizeof(ts));
      seq++;

      int64_t window_id = db1_window_create_raw(session_id, seq, summary, ts);

      if (window_id > 0)
      {
         for (int d = 0; d < decision_count; d++)
            (void)db1_decision_record(window_id, decision_descs[d], ts);

         for (int t = 0; t < term_count; t++)
            (void)db1_window_add_term(window_id, terms[t]);

         for (int f = 0; f < file_ref_count; f++)
            (void)db1_window_add_file(window_id, file_refs[f]);

         (void)db1_window_index_summary(window_id, summary);

         memory_extract_edges(window_id, file_refs, file_ref_count, terms, term_count);
         windows_added++;
      }

      scan_free_message_parts(message_parts, &message_count);
      for (int i = 0; i < term_count; i++)
         free(terms[i]);
      for (int i = 0; i < file_ref_count; i++)
         free(file_refs[i]);
   }
   else
      scan_free_message_parts(message_parts, &message_count);

   free(line_buf);
   fclose(fp);
   return windows_added;
}

/* --- Recursive directory walk for .jsonl files --- */

static void walk_dir(const char *dir_path, int *total_windows)
{
   DIR *dir = opendir(dir_path);
   if (!dir)
      return;

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_name[0] == '.')
         continue;

      char full_path[MAX_PATH_LEN];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

      struct stat st;
      if (stat(full_path, &st) != 0)
         continue;

      if (S_ISDIR(st.st_mode))
      {
         walk_dir(full_path, total_windows);
         continue;
      }

      if (!S_ISREG(st.st_mode))
         continue;

      /* Check .jsonl extension */
      const char *ext = strrchr(entry->d_name, '.');
      if (!ext || strcmp(ext, ".jsonl") != 0)
         continue;

      /* Use directory name as session_id if possible */
      char session_id[128];
      const char *parent = strrchr(dir_path, '/');
      if (parent)
         snprintf(session_id, sizeof(session_id), "%s", parent + 1);
      else
         snprintf(session_id, sizeof(session_id), "%s", dir_path);

      int added = scan_file(session_id, full_path);
      if (added > 0 && total_windows)
         *total_windows += added;
   }

   closedir(dir);
}

int memory_scan_conversations(char dirs[][MAX_PATH_LEN], int dir_count)
{
   if (!db1_windows_session_scan_state || !db1_window_create_raw || !db1_decision_record ||
       !db1_window_add_term || !db1_window_add_file || !db1_window_index_summary)
      return -1;

   int total = 0;

   for (int i = 0; i < dir_count; i++)
   {
      struct stat st;
      if (stat(dirs[i], &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      walk_dir(dirs[i], &total);
   }

   return total;
}

/* === Entity Profile Cards === */

/* memory_is_profile_query: return 1 if the query is profile-shaped.
 *
 * Profile-shaped queries ask about what kind of person an entity is,
 * their communication style, preferences, or characteristics.
 * Examples: "what kind of person is Caroline?",
 *           "how does Melanie communicate?",
 *           "what are Jordan's food preferences?".
 */
int memory_is_profile_query(const char *query)
{
   if (!query || !query[0])
      return 0;

   /* Profile intent phrases */
   static const char *phrases[] = {
       "what kind of person",
       "what type of person",
       "what sort of person",
       "how does",
       "how do",
       "what are",
       "what is",
       "what's",
       "tell me about",
       "describe",
       "personality",
       "character",
       "communication style",
       "food preference",
       "preferences",
       "traits",
       "routines",
       "habits",
       "relationships",
       NULL,
   };

   /* Secondary signals that confirm entity-profile intent */
   static const char *entity_signals[] = {
       "communicate", "talk", "speak",  "like",          "loves", "enjoys",
       "prefer",      "fear", "worry",  "interested in", "hobby", "hobbies",
       "personality", "feel", "thinks", "believes",      NULL,
   };

   /* Quick check: must mention an entity (capitalized word or possessive) */
   /* First check for phrase + entity signal combination */
   for (int i = 0; phrases[i]; i++)
   {
      if (strcasestr(query, phrases[i]))
      {
         for (int j = 0; entity_signals[j]; j++)
         {
            if (strcasestr(query, entity_signals[j]))
               return 1;
         }
      }
   }

   /* Explicit profile patterns */
   if (strcasestr(query, "what kind of person") || strcasestr(query, "what type of person") ||
       strcasestr(query, "what sort of person"))
      return 1;
   if (strcasestr(query, "communication style") || strcasestr(query, "communicates with"))
      return 1;
   if (strcasestr(query, "food preference") || strcasestr(query, "dietary preference") ||
       strcasestr(query, "food preferences"))
      return 1;

   return 0;
}

/* Build a profile card for entity_id using aggregation (no LLM).
 * Aggregates: recurring_topics and relationships from entity_edges,
 * observation count from memory_entities.
 * Writes a JSON string into out_json. Returns 0 on success, -1 on failure.
 */
int memory_profile_card_build(const char *entity_id, int min_obs, char *out_json,
                              size_t out_json_len)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)entity_id;
   (void)min_obs;
   (void)out_json;
   (void)out_json_len;
   return -1;
#else
   if (!entity_id || !entity_id[0] || !out_json || out_json_len == 0)
      return -1;

   int obs_count = db2_entity_count_observations(entity_id);
   if (obs_count < min_obs)
      return -1;

   /* Gather recurring_topics: top co_discussed targets by weight. */
   db2_entity_neighbor_t topics_buf[8];
   int topic_count =
       db2_entity_edge_top_targets_by_relation(entity_id, "co_discussed", topics_buf, 8);

   /* Gather relationships: entities this entity co-edited with most. */
   db2_entity_neighbor_t rels_buf[6];
   int rel_count = db2_entity_edge_top_partners_by_relation(entity_id, "co_edited", rels_buf, 6);

   /* Build JSON using cJSON */
   cJSON *card = cJSON_CreateObject();
   if (!card)
      return -1;

   cJSON_AddStringToObject(card, "entity_id", entity_id);
   cJSON_AddStringToObject(card, "canonical_name", entity_id);
   cJSON_AddNumberToObject(card, "observation_count", obs_count);

   cJSON *topics_arr = cJSON_CreateArray();
   for (int i = 0; i < topic_count; i++)
      cJSON_AddItemToArray(topics_arr, cJSON_CreateString(topics_buf[i].node));
   cJSON_AddItemToObject(card, "recurring_topics", topics_arr);

   cJSON *rels_arr = cJSON_CreateArray();
   for (int i = 0; i < rel_count; i++)
   {
      cJSON *rel_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(rel_obj, "with", rels_buf[i].node);
      cJSON_AddStringToObject(rel_obj, "kind", "co_edited");
      cJSON_AddItemToArray(rels_arr, rel_obj);
   }
   cJSON_AddItemToObject(card, "relationships", rels_arr);

   char *json_str = cJSON_PrintUnformatted(card);
   cJSON_Delete(card);

   if (!json_str)
      return -1;

   snprintf(out_json, out_json_len, "%s", json_str);
   free(json_str);
   return 0;
#endif
}

/* Profile-card upsert moved to db2/entity_profiles.c
 * (db2_entity_profile_upsert). */

/* Refresh stale profile cards.
 * Finds entities with >= min_obs observations whose card is older than
 * stale_secs seconds (or has no card yet) and rebuilds them.
 * Returns the number of cards refreshed.
 */
int memory_profile_card_refresh(int min_obs, int stale_secs)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)min_obs;
   (void)stale_secs;
   return 0;
#else
   if (min_obs <= 0)
      min_obs = 10;
   if (stale_secs <= 0)
      stale_secs = 86400;

   char entities[64][128];
   int obs_counts[64];
   int ent_count = db2_entity_list_active(min_obs, entities, obs_counts, 64);

   int refreshed = 0;
   char stale_cutoff[32];
   snprintf(stale_cutoff, sizeof(stale_cutoff), "-%d seconds", stale_secs);

   for (int i = 0; i < ent_count; i++)
   {
      if (db2_entity_profile_is_fresh(entities[i], stale_cutoff) == 1)
         continue;

      char card_json[8192];
      if (memory_profile_card_build(entities[i], min_obs, card_json, sizeof(card_json)) == 0)
      {
         if (db2_entity_profile_upsert(entities[i], entities[i], obs_counts[i], card_json) == 0)
            refreshed++;
      }
   }

   return refreshed;
#endif
}

/* Retrieve a stored profile card JSON for an entity.
 * Returns 0 on success (card exists), -1 if not found.
 */
int memory_profile_card_get(const char *entity, char *out_json, size_t out_json_len)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)entity;
   (void)out_json;
   (void)out_json_len;
   return -1;
#else
   return db2_entity_profile_get_card(entity, out_json, out_json_len);
#endif
}
