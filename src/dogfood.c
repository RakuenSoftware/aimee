/* dogfood.c: fire-and-forget JSONL logger for memory-adjacent tool
 * moments. One file per month at `<log_dir>/YYYY-MM.jsonl`. Privacy
 * default is hash-only queries; raw text is opt-in. See
 * docs/proposals/pending/dogfood-agent-eval.md. */

#include "aimee.h"
#include "dogfood.h"
#include "modules/learning/learning_implicit.h"
#include "platform_path.h"
#include "report_enrichment.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int64_t g_records_written = 0;
static int64_t g_failures = 0;

/* Process-scope handoff for the REPAIR / CONTINUATION auto-label
 * heuristic: dogfood_log_moment_impl stashes the id of the record it
 * just wrote; dogfood_autolabel_next_turn_live() consumes and clears
 * it so at most one label is applied per user turn. */
static char g_last_record_id[24] = "";
static char g_last_record_month[8] = "";

/* FNV-1a 32-bit: short, non-cryptographic, stable across runs. Stable
 * hashes let monthly reports dedupe equivalent queries without keeping
 * the raw text. */
static uint32_t fnv1a32(const char *s)
{
   uint32_t h = 2166136261u;
   if (!s)
      return h;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
   {
      h ^= *p;
      h *= 16777619u;
   }
   return h;
}

/* Resolve the dogfood settings from the live config. Replaces
 * dogfood_config_from(const legacy_config_record *, ...), which every caller reached by
 * loading a whole legacy_config_record and immediately projecting seven fields out of it. */
void dogfood_config_current(dogfood_config_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->enabled = config_dogfood_enabled();
   out->commit_raw = config_dogfood_commit_raw();
   out->inline_tagging = config_dogfood_inline_tagging();
   out->autolabel_repair = config_dogfood_autolabel_repair();
   out->autolabel_continuation = config_dogfood_autolabel_continuation();
   out->autolabel_repeat_question = config_dogfood_autolabel_repeat_question();
   const char *dir = config_dogfood_log_dir();
   if (dir[0])
      snprintf(out->log_dir, sizeof(out->log_dir), "%s", dir);
   else
      snprintf(out->log_dir, sizeof(out->log_dir), "%s/dogfood", config_output_dir());
}

char *dogfood_inline_hint_json(const dogfood_config_t *cfg, const char *record_id, const char *tool)
{
   if (!cfg || !cfg->inline_tagging || !record_id || !record_id[0])
      return NULL;
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddStringToObject(obj, "kind", "dogfood_tag_prompt");
   cJSON_AddStringToObject(obj, "record_id", record_id);
   if (tool && tool[0])
      cJSON_AddStringToObject(obj, "tool", tool);
   cJSON *outcomes = cJSON_AddArrayToObject(obj, "outcomes");
   if (outcomes)
   {
      cJSON_AddItemToArray(outcomes, cJSON_CreateString("hit"));
      cJSON_AddItemToArray(outcomes, cJSON_CreateString("partial"));
      cJSON_AddItemToArray(outcomes, cJSON_CreateString("miss"));
      cJSON_AddItemToArray(outcomes, cJSON_CreateString("hallucination"));
   }
   cJSON_AddStringToObject(obj, "tag_command", "aimee dogfood tag");
   char *out = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return out;
}

void dogfood_log_moment_live(const char *tool, const char *query, const int64_t *retrieved_ids,
                             int retrieved_count, const char *notes)
{
   dogfood_config_t dcfg;
   dogfood_config_current(&dcfg);
   dogfood_log_moment(&dcfg, tool, query, retrieved_ids, retrieved_count, notes);
}

void dogfood_metrics(int64_t *records_out, int64_t *failures_out)
{
   if (records_out)
      *records_out = g_records_written;
   if (failures_out)
      *failures_out = g_failures;
}

void dogfood_metrics_reset(void)
{
   g_records_written = 0;
   g_failures = 0;
}

/* Deterministic record id derived from (ts, session_id, tool,
 * query_hash). Not cryptographic; collisions are fine — two records
 * with identical fields in the same second refer to the same moment. */
static void compute_record_id(const char *ts, const char *sid, const char *tool,
                              const char *query_hash, char *out, size_t cap)
{
   if (!out || cap < 9)
      return;
   uint32_t h = 2166136261u;
   const char *parts[] = {ts, sid, tool, query_hash, NULL};
   for (int p = 0; parts[p]; p++)
   {
      for (const unsigned char *c = (const unsigned char *)parts[p]; *c; c++)
      {
         h ^= *c;
         h *= 16777619u;
      }
      h ^= ':';
      h *= 16777619u;
   }
   /* 8 hex chars of FNV + 8 hex chars of a second hash for a stable,
    * low-collision 16-char id. */
   uint32_t h2 = h ^ 0xdeadbeefu;
   h2 *= 16777619u;
   for (const unsigned char *c = (const unsigned char *)(sid ? sid : ""); *c; c++)
      h2 = (h2 ^ *c) * 16777619u;
   snprintf(out, cap, "%08x%08x", (unsigned)h, (unsigned)h2);
}

static void dogfood_log_moment_impl(const dogfood_config_t *cfg, const char *tool,
                                    const char *query, const int64_t *retrieved_ids,
                                    int retrieved_count, const char *notes,
                                    const dogfood_label_t *label, char *record_id_out,
                                    size_t record_id_cap)
{
   if (record_id_out && record_id_cap > 0)
      record_id_out[0] = '\0';

   if (!cfg || !cfg->enabled || !tool || !tool[0])
      return;
   if (!cfg->log_dir[0])
   {
      g_failures++;
      return;
   }
   if (platform_mkdir_p(cfg->log_dir, 0700) != 0)
   {
      g_failures++;
      return;
   }

   char ts[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   char month[8];
   strftime(month, sizeof(month), "%Y-%m", &tm_buf);

   char path[640];
   snprintf(path, sizeof(path), "%s/%s.jsonl", cfg->log_dir, month);

   char query_hash[12];
   query_hash[0] = '\0';
   if (query && query[0])
      snprintf(query_hash, sizeof(query_hash), "%08x", (unsigned)fnv1a32(query));

   const char *sid = session_id();
   char rid[24];
   compute_record_id(ts, sid, tool, query_hash, rid, sizeof(rid));

   cJSON *rec = cJSON_CreateObject();
   if (!rec)
   {
      g_failures++;
      return;
   }
   cJSON_AddStringToObject(rec, "id", rid);
   cJSON_AddStringToObject(rec, "ts", ts);
   cJSON_AddStringToObject(rec, "session_id", sid);
   cJSON_AddStringToObject(rec, "tool", tool);

   if (query_hash[0])
   {
      cJSON_AddStringToObject(rec, "query_hash", query_hash);
      if (cfg->commit_raw)
         cJSON_AddStringToObject(rec, "query", query);
   }

   cJSON *ids = cJSON_CreateArray();
   if (ids && retrieved_ids && retrieved_count > 0)
   {
      for (int i = 0; i < retrieved_count; i++)
         cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)retrieved_ids[i]));
   }
   cJSON_AddItemToObject(rec, "retrieved_memory_ids", ids ? ids : cJSON_CreateArray());
   cJSON_AddNumberToObject(rec, "retrieved_count", retrieved_count > 0 ? retrieved_count : 0);

   if (notes && notes[0])
      cJSON_AddStringToObject(rec, "notes", notes);

   /* Auto-applied labels land inline on the record itself; post-hoc
    * labels (dogfood_label_record) go in the sidecar. */
   if (label)
   {
      if (label->outcome && label->outcome[0])
         cJSON_AddStringToObject(rec, "outcome", label->outcome);
      if (label->context_richness > 0 && label->context_richness <= 5)
         cJSON_AddNumberToObject(rec, "context_richness", label->context_richness);
      if (label->has_surprise)
         cJSON_AddBoolToObject(rec, "surprise", label->surprise ? 1 : 0);
      if (label->prospective_surfaced)
         cJSON_AddBoolToObject(rec, "prospective_surfaced", 1);
      if (label->notes && label->notes[0] && !(notes && notes[0]))
         cJSON_AddStringToObject(rec, "notes", label->notes);
   }

   /* Write-time repeat_question heuristic: if the same
    * (session, tool, query_hash) triple already appeared in this
    * month's log, mark the incoming record outcome=miss. Skip when
    * the record already carries an outcome from the caller or lacks a
    * query hash. */
   int repeat_labelled = 0;
   if (cfg->autolabel_repeat_question && query_hash[0] &&
       !cJSON_GetObjectItemCaseSensitive(rec, "outcome") &&
       dogfood_query_is_repeat(cfg, sid, tool, query_hash))
   {
      cJSON_AddStringToObject(rec, "outcome", "miss");
      cJSON_AddStringToObject(rec, "autolabel_source", "repeat_question");
      if (!cJSON_GetObjectItemCaseSensitive(rec, "notes"))
         cJSON_AddStringToObject(rec, "notes", "autolabel: repeat question in month");
      repeat_labelled = 1;
   }
   (void)repeat_labelled;
   if (query_hash[0])
      learning_implicit_record_repeat_question(sid, tool, query_hash);

   char *line = cJSON_PrintUnformatted(rec);
   cJSON_Delete(rec);
   if (!line)
   {
      g_failures++;
      return;
   }

   FILE *fp = fopen(path, "a");
   if (!fp)
   {
      free(line);
      g_failures++;
      return;
   }
   if (fprintf(fp, "%s\n", line) < 0)
      g_failures++;
   else
   {
      g_records_written++;
      if (record_id_out && record_id_cap > 0)
         snprintf(record_id_out, record_id_cap, "%s", rid);
      /* Stash for the REPAIR / CONTINUATION live hook. Records already
       * auto-labelled at write time (repeat_question) aren't eligible
       * for the per-turn hook — clear the handoff. */
      if (!repeat_labelled)
      {
         snprintf(g_last_record_id, sizeof(g_last_record_id), "%s", rid);
         snprintf(g_last_record_month, sizeof(g_last_record_month), "%s", month);
      }
      else
      {
         g_last_record_id[0] = '\0';
         g_last_record_month[0] = '\0';
      }
   }
   fclose(fp);
   free(line);
}

void dogfood_log_moment(const dogfood_config_t *cfg, const char *tool, const char *query,
                        const int64_t *retrieved_ids, int retrieved_count, const char *notes)
{
   dogfood_log_moment_impl(cfg, tool, query, retrieved_ids, retrieved_count, notes, NULL, NULL, 0);
}

void dogfood_log_moment_with_id(const dogfood_config_t *cfg, const char *tool, const char *query,
                                const int64_t *retrieved_ids, int retrieved_count,
                                const char *notes, char *record_id_out, size_t record_id_cap)
{
   dogfood_log_moment_impl(cfg, tool, query, retrieved_ids, retrieved_count, notes, NULL,
                           record_id_out, record_id_cap);
}

void dogfood_log_moment_tagged(const char *tool, const char *query, const int64_t *retrieved_ids,
                               int retrieved_count, const dogfood_label_t *label)
{
   dogfood_config_t dcfg;
   dogfood_config_current(&dcfg);
   dogfood_log_moment_impl(&dcfg, tool, query, retrieved_ids, retrieved_count, NULL, label, NULL,
                           0);
}

static ssize_t dogfood_read_line(char **linep, size_t *cap, FILE *fp)
{
   if (!linep || !cap || !fp)
      return -1;

   if (!*linep || *cap == 0)
   {
      *cap = 256;
      *linep = malloc(*cap);
      if (!*linep)
      {
         *cap = 0;
         return -1;
      }
   }

   size_t len = 0;
   int ch;
   while ((ch = fgetc(fp)) != EOF)
   {
      if (len + 1 >= *cap)
      {
         size_t next_cap = *cap * 2;
         char *next = realloc(*linep, next_cap);
         if (!next)
            return -1;
         *linep = next;
         *cap = next_cap;
      }
      (*linep)[len++] = (char)ch;
      if (ch == '\n')
         break;
   }

   if (len == 0 && ch == EOF)
      return -1;

   (*linep)[len] = '\0';
   return (ssize_t)len;
}

/* Read all lines of a JSONL file; return a cJSON array. Malformed
 * lines are skipped so a corrupted tail doesn't lose the whole month. */
static cJSON *dogfood_read_jsonl(const char *path)
{
   FILE *fp = fopen(path, "r");
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      if (fp)
         fclose(fp);
      return NULL;
   }
   if (!fp)
      return arr;

   char *line = NULL;
   size_t cap = 0;
   ssize_t n;
   while ((n = dogfood_read_line(&line, &cap, fp)) > 0)
   {
      while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
         line[--n] = '\0';
      if (n == 0)
         continue;
      cJSON *obj = cJSON_Parse(line);
      if (obj)
         cJSON_AddItemToArray(arr, obj);
   }
   free(line);
   fclose(fp);
   return arr;
}

cJSON *dogfood_read_month(const char *dir, const char *month)
{
   if (!dir || !month)
      return NULL;
   char rpath[640], lpath[640];
   snprintf(rpath, sizeof(rpath), "%s/%s.jsonl", dir, month);
   snprintf(lpath, sizeof(lpath), "%s/%s.labels.jsonl", dir, month);

   cJSON *records = dogfood_read_jsonl(rpath);
   cJSON *labels = dogfood_read_jsonl(lpath);
   if (!records)
   {
      cJSON_Delete(labels);
      return NULL;
   }

   int record_count = cJSON_GetArraySize(records);
   int bucket_count = 16;
   while (bucket_count < record_count * 2 && bucket_count < (1 << 20))
      bucket_count <<= 1;

   typedef struct
   {
      const char *id;
      cJSON *record;
      int next;
   } record_index_entry_t;

   int *buckets = calloc((size_t)bucket_count, sizeof(*buckets));
   record_index_entry_t *entries = calloc((size_t)record_count, sizeof(*entries));
   int entry_count = 0;
   if (buckets && entries)
   {
      for (int i = 0; i < bucket_count; i++)
         buckets[i] = -1;

      cJSON *rec;
      cJSON_ArrayForEach(rec, records)
      {
         cJSON *rid = cJSON_GetObjectItemCaseSensitive(rec, "id");
         if (!cJSON_IsString(rid) || !rid->valuestring[0] || entry_count >= record_count)
            continue;

         uint64_t h = 1469598103934665603ULL;
         for (const unsigned char *p = (const unsigned char *)rid->valuestring; *p; p++)
         {
            h ^= *p;
            h *= 1099511628211ULL;
         }
         int b = (int)(h & (uint64_t)(bucket_count - 1));
         entries[entry_count].id = rid->valuestring;
         entries[entry_count].record = rec;
         entries[entry_count].next = buckets[b];
         buckets[b] = entry_count++;
      }
   }

   cJSON *lbl;
   cJSON_ArrayForEach(lbl, labels)
   {
      cJSON *lid = cJSON_GetObjectItemCaseSensitive(lbl, "id");
      if (!cJSON_IsString(lid))
         continue;
      cJSON *rec = NULL;
      if (buckets && entries)
      {
         uint64_t h = 1469598103934665603ULL;
         for (const unsigned char *p = (const unsigned char *)lid->valuestring; *p; p++)
         {
            h ^= *p;
            h *= 1099511628211ULL;
         }
         int b = (int)(h & (uint64_t)(bucket_count - 1));
         for (int e = buckets[b]; e >= 0; e = entries[e].next)
         {
            if (strcmp(entries[e].id, lid->valuestring) == 0)
            {
               rec = entries[e].record;
               break;
            }
         }
      }
      else
      {
         cJSON_ArrayForEach(rec, records)
         {
            cJSON *rid = cJSON_GetObjectItemCaseSensitive(rec, "id");
            if (cJSON_IsString(rid) && strcmp(rid->valuestring, lid->valuestring) == 0)
               break;
         }
      }
      if (!rec)
         continue;

      const char *copy_fields[] = {"outcome", "context_richness",     "surprise",
                                   "notes",   "prospective_surfaced", "autolabel_source",
                                   NULL};
      for (int f = 0; copy_fields[f]; f++)
      {
         cJSON *v = cJSON_GetObjectItemCaseSensitive(lbl, copy_fields[f]);
         if (!v)
            continue;
         cJSON_DeleteItemFromObjectCaseSensitive(rec, copy_fields[f]);
         cJSON_AddItemToObject(rec, copy_fields[f], cJSON_Duplicate(v, 1));
      }
   }
   free(entries);
   free(buckets);
   cJSON_Delete(labels);
   return records;
}

static void dogfood_bucket_bump(cJSON *bucket, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(bucket, key);
   if (cJSON_IsNumber(v))
      cJSON_SetNumberValue(v, v->valueint + 1);
   else
      cJSON_AddNumberToObject(bucket, key, 1);
}

cJSON *dogfood_build_report(const cJSON *records)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;

   cJSON *per_tool = cJSON_AddObjectToObject(out, "per_tool");
   cJSON *outcomes = cJSON_AddObjectToObject(out, "outcomes");
   cJSON *richness = cJSON_AddObjectToObject(out, "context_richness");
   cJSON *surprise_wins = cJSON_AddArrayToObject(out, "surprise_wins");
   cJSON *hard_negatives = cJSON_AddArrayToObject(out, "hard_negatives");
   cJSON *prospective = cJSON_AddArrayToObject(out, "prospective_surfaced");
   cJSON *auto_labels = cJSON_AddObjectToObject(out, "auto_labels");

   int total = 0;
   int labelled = 0;
   int auto_labelled = 0;
   int retrieved_zero = 0;

   const cJSON *rec;
   cJSON_ArrayForEach(rec, records)
   {
      total++;
      const cJSON *tool = cJSON_GetObjectItemCaseSensitive(rec, "tool");
      const char *tn = cJSON_IsString(tool) ? tool->valuestring : "unknown";
      dogfood_bucket_bump(per_tool, tn);

      const cJSON *rcount = cJSON_GetObjectItemCaseSensitive(rec, "retrieved_count");
      if (cJSON_IsNumber(rcount) && rcount->valueint == 0)
         retrieved_zero++;

      const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(rec, "outcome");
      if (cJSON_IsString(outcome))
      {
         labelled++;
         dogfood_bucket_bump(outcomes, outcome->valuestring);
         if (strcmp(outcome->valuestring, "miss") == 0 ||
             strcmp(outcome->valuestring, "hallucination") == 0)
            cJSON_AddItemToArray(hard_negatives, cJSON_Duplicate(rec, 1));
         if (strcmp(outcome->valuestring, "hit") == 0)
         {
            const cJSON *surp = cJSON_GetObjectItemCaseSensitive(rec, "surprise");
            if (cJSON_IsBool(surp) && cJSON_IsTrue(surp))
               cJSON_AddItemToArray(surprise_wins, cJSON_Duplicate(rec, 1));
         }
      }
      else
      {
         dogfood_bucket_bump(outcomes, "unlabelled");
      }

      const cJSON *rn = cJSON_GetObjectItemCaseSensitive(rec, "context_richness");
      if (cJSON_IsNumber(rn))
      {
         char key[4];
         snprintf(key, sizeof(key), "%d", rn->valueint);
         dogfood_bucket_bump(richness, key);
      }

      const cJSON *pros = cJSON_GetObjectItemCaseSensitive(rec, "prospective_surfaced");
      if (cJSON_IsBool(pros) && cJSON_IsTrue(pros))
         cJSON_AddItemToArray(prospective, cJSON_Duplicate(rec, 1));

      const cJSON *auto_src = cJSON_GetObjectItemCaseSensitive(rec, "autolabel_source");
      if (cJSON_IsString(auto_src))
      {
         auto_labelled++;
         dogfood_bucket_bump(auto_labels, auto_src->valuestring);
      }
   }

   cJSON_AddNumberToObject(out, "records_total", total);
   cJSON_AddNumberToObject(out, "records_labelled", labelled);
   cJSON_AddNumberToObject(out, "records_auto_labelled", auto_labelled);
   cJSON_AddNumberToObject(out, "records_retrieved_zero", retrieved_zero);
   return out;
}

/* --- Proposal signal parsing + classification --- */

/* Trim ASCII whitespace in place and return the pointer to the first
 * non-space character. */
static char *str_trim(char *s)
{
   while (*s && isspace((unsigned char)*s))
      s++;
   if (!*s)
      return s;
   char *end = s + strlen(s) - 1;
   while (end > s && isspace((unsigned char)*end))
      *end-- = '\0';
   return s;
}

/* Parse one `key=value` or `count>=N` fragment. On `count>=N` the
 * count is stored in *count_out and 1 is returned; otherwise a
 * predicate entry is added to `predicates`. Invalid fragments are
 * skipped silently so a malformed proposal doesn't poison the whole
 * report. */
static void parse_predicate_fragment(char *frag, cJSON *predicates, int *count_out)
{
   frag = str_trim(frag);
   if (!*frag)
      return;
   /* count>=N */
   char *ge = strstr(frag, ">=");
   if (ge)
   {
      *ge = '\0';
      char *key = str_trim(frag);
      char *val = str_trim(ge + 2);
      if (strcmp(key, "count") == 0 && val[0])
      {
         int n = atoi(val);
         if (n > 0)
            *count_out = n;
      }
      return;
   }
   char *eq = strchr(frag, '=');
   if (!eq)
      return;
   *eq = '\0';
   char *key = str_trim(frag);
   char *val = str_trim(eq + 1);
   if (!key[0])
      return;
   cJSON_AddStringToObject(predicates, key, val);
}

cJSON *dogfood_parse_signals_md(const char *markdown)
{
   if (!markdown)
      return NULL;
   const char *header = strstr(markdown, "## Dogfood Signals");
   if (!header)
      return NULL;
   const char *start = strchr(header, '\n');
   if (!start)
      return NULL;
   start++;

   /* Block ends at next `## ` heading or EOF. */
   const char *end = start;
   while (*end)
   {
      if (end[0] == '\n' && end[1] == '#' && end[2] == '#' && end[3] == ' ')
      {
         end++;
         break;
      }
      end++;
   }

   cJSON *out = cJSON_CreateArray();
   if (!out)
      return NULL;

   const char *p = start;
   while (p < end)
   {
      const char *nl = strchr(p, '\n');
      if (!nl || nl > end)
         nl = end;
      size_t llen = (size_t)(nl - p);
      char line[512];
      if (llen >= sizeof(line))
         llen = sizeof(line) - 1;
      memcpy(line, p, llen);
      line[llen] = '\0';
      char *trimmed = str_trim(line);

      const char *kind = NULL;
      char *body = NULL;
      if (strncmp(trimmed, "- confirm:", 10) == 0)
      {
         kind = "confirm";
         body = trimmed + 10;
      }
      else if (strncmp(trimmed, "- contradict:", 13) == 0)
      {
         kind = "contradict";
         body = trimmed + 13;
      }

      if (kind && body)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON *predicates = cJSON_CreateObject();
         int count = 1;
         char *saveptr = NULL;
         char *tok = strtok_r(body, ",", &saveptr);
         while (tok)
         {
            parse_predicate_fragment(tok, predicates, &count);
            tok = strtok_r(NULL, ",", &saveptr);
         }
         cJSON_AddStringToObject(entry, "kind", kind);
         cJSON_AddItemToObject(entry, "predicates", predicates);
         cJSON_AddNumberToObject(entry, "count", count);
         cJSON_AddItemToArray(out, entry);
      }

      p = (nl == end) ? end : nl + 1;
   }

   return out;
}

/* Match a single predicate value against a cJSON record field. */
static int predicate_matches(const cJSON *rec, const char *key, const char *want)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(rec, key);
   if (!v)
      return 0;
   if (cJSON_IsString(v))
      return strcmp(v->valuestring, want) == 0;
   if (cJSON_IsBool(v))
   {
      int b = cJSON_IsTrue(v) ? 1 : 0;
      return (strcmp(want, "true") == 0 && b) || (strcmp(want, "false") == 0 && !b);
   }
   if (cJSON_IsNumber(v))
   {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", v->valueint);
      return strcmp(buf, want) == 0;
   }
   return 0;
}

static int signal_match_count(const cJSON *records, const cJSON *signal)
{
   const cJSON *preds = cJSON_GetObjectItemCaseSensitive(signal, "predicates");
   int hits = 0;
   const cJSON *rec;
   cJSON_ArrayForEach(rec, records)
   {
      int ok = 1;
      const cJSON *p;
      cJSON_ArrayForEach(p, preds)
      {
         if (!cJSON_IsString(p) || !p->string)
         {
            ok = 0;
            break;
         }
         if (!predicate_matches(rec, p->string, p->valuestring))
         {
            ok = 0;
            break;
         }
      }
      if (ok)
         hits++;
   }
   return hits;
}

const char *dogfood_classify(const cJSON *records, const cJSON *signals)
{
   if (!records || !signals)
      return "no-signal";
   int confirm_hits = 0;
   int contradict_hits = 0;
   int confirm_any = 0;
   int contradict_any = 0;

   const cJSON *sig;
   cJSON_ArrayForEach(sig, signals)
   {
      const cJSON *kind = cJSON_GetObjectItemCaseSensitive(sig, "kind");
      const cJSON *need = cJSON_GetObjectItemCaseSensitive(sig, "count");
      if (!cJSON_IsString(kind))
         continue;
      int threshold = cJSON_IsNumber(need) && need->valueint > 0 ? need->valueint : 1;
      int hits = signal_match_count(records, sig);
      if (strcmp(kind->valuestring, "confirm") == 0)
      {
         confirm_any = 1;
         if (hits >= threshold)
            confirm_hits++;
      }
      else if (strcmp(kind->valuestring, "contradict") == 0)
      {
         contradict_any = 1;
         if (hits >= threshold)
            contradict_hits++;
      }
   }

   if (contradict_hits > 0)
      return "contradicted";
   if (confirm_hits > 0)
      return "confirmed";
   if (!confirm_any && !contradict_any)
      return "no-signal";
   return "no-signal";
}

/* --- Weak auto-labelling heuristics --- */

/* Correction cues — the first token (or leading phrase) of the user's
 * next turn that signals "that answer was wrong". Kept narrow so a
 * drive-by "not bad" doesn't flip a hit to a miss. */
static const char *CORRECTION_CUES[] = {
    "no ",          "no,",         "no.",           "no!",       "nope",
    "actually",     "wrong",       "incorrect",     "not quite", "not right",
    "that's wrong", "thats wrong", "that is wrong", "not true",  NULL,
};

/* Lowercase-compare prefix of `s` against `prefix`. */
static int starts_with_ci(const char *s, const char *prefix)
{
   for (size_t i = 0; prefix[i]; i++)
   {
      if (!s[i])
         return 0;
      char a = (char)tolower((unsigned char)s[i]);
      char b = (char)tolower((unsigned char)prefix[i]);
      if (a != b)
         return 0;
   }
   return 1;
}

dogfood_autolabel_kind_t dogfood_classify_next_turn(const char *text)
{
   if (!text)
      return DOGFOOD_AUTOLABEL_NONE;
   /* Skip leading whitespace and common punctuation. */
   const char *p = text;
   while (*p && (isspace((unsigned char)*p) || *p == '>' || *p == '-'))
      p++;
   if (!*p)
      return DOGFOOD_AUTOLABEL_NONE;

   for (int i = 0; CORRECTION_CUES[i]; i++)
   {
      if (starts_with_ci(p, CORRECTION_CUES[i]))
         return DOGFOOD_AUTOLABEL_REPAIR;
   }

   /* Count substantive characters to keep short acks ("ok", "thx")
    * out of CONTINUATION — they're too ambiguous to treat as a hit. */
   int chars = 0;
   for (const char *q = p; *q; q++)
   {
      if (!isspace((unsigned char)*q))
         chars++;
      if (chars >= 3)
         break;
   }
   if (chars < 3)
      return DOGFOOD_AUTOLABEL_NONE;
   return DOGFOOD_AUTOLABEL_CONTINUATION;
}

int dogfood_autolabel_apply(const dogfood_config_t *cfg_in, const char *record_id,
                            dogfood_autolabel_kind_t kind)
{
   if (kind == DOGFOOD_AUTOLABEL_NONE)
      return 0;
   if (!record_id || !record_id[0])
      return -1;

   dogfood_config_t live;
   const dogfood_config_t *cfg = cfg_in;
   if (!cfg)
   {
      dogfood_config_current(&live);
      cfg = &live;
   }
   if (!cfg->enabled || !cfg->log_dir[0])
      return -1;
   if (platform_mkdir_p(cfg->log_dir, 0700) != 0)
   {
      g_failures++;
      return -1;
   }

   char ts[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   char month[8];
   /* Honour the month the record was written in when the live variant
    * called us right after the write. Otherwise use current UTC — a
    * month-boundary autolabel is a rare edge case. */
   if (g_last_record_month[0] && strcmp(record_id, g_last_record_id) == 0)
      snprintf(month, sizeof(month), "%s", g_last_record_month);
   else
      strftime(month, sizeof(month), "%Y-%m", &tm_buf);

   char path[640];
   snprintf(path, sizeof(path), "%s/%s.labels.jsonl", cfg->log_dir, month);

   cJSON *rec = cJSON_CreateObject();
   if (!rec)
   {
      g_failures++;
      return -1;
   }
   cJSON_AddStringToObject(rec, "ts", ts);
   cJSON_AddStringToObject(rec, "id", record_id);

   const char *outcome = NULL;
   const char *source = NULL;
   const char *note = NULL;
   switch (kind)
   {
   case DOGFOOD_AUTOLABEL_REPAIR:
      outcome = "miss";
      source = "repair";
      note = "autolabel: repair (correction cue on next turn)";
      break;
   case DOGFOOD_AUTOLABEL_CONTINUATION:
      outcome = "hit";
      source = "continuation";
      note = "autolabel: continuation (advancing next turn)";
      break;
   default:
      cJSON_Delete(rec);
      return -1;
   }
   cJSON_AddStringToObject(rec, "outcome", outcome);
   cJSON_AddStringToObject(rec, "autolabel_source", source);
   if (kind == DOGFOOD_AUTOLABEL_CONTINUATION)
      cJSON_AddBoolToObject(rec, "surprise", 0);
   cJSON_AddStringToObject(rec, "notes", note);

   char *line = cJSON_PrintUnformatted(rec);
   cJSON_Delete(rec);
   if (!line)
   {
      g_failures++;
      return -1;
   }

   FILE *fp = fopen(path, "a");
   if (!fp)
   {
      free(line);
      g_failures++;
      return -1;
   }
   int rc = fprintf(fp, "%s\n", line) < 0 ? -1 : 0;
   fclose(fp);
   free(line);
   if (rc != 0)
      g_failures++;
   return rc;
}

void dogfood_autolabel_next_turn_live(const char *text)
{
   if (!g_last_record_id[0])
      return;
   dogfood_config_t cfg;
   dogfood_config_current(&cfg);
   if (!cfg.enabled)
      return;
   if (!cfg.autolabel_repair && !cfg.autolabel_continuation)
      return;

   dogfood_autolabel_kind_t kind = dogfood_classify_next_turn(text);
   if (kind == DOGFOOD_AUTOLABEL_REPAIR && !cfg.autolabel_repair)
      kind = DOGFOOD_AUTOLABEL_NONE;
   if (kind == DOGFOOD_AUTOLABEL_CONTINUATION && !cfg.autolabel_continuation)
      kind = DOGFOOD_AUTOLABEL_NONE;
   if (kind == DOGFOOD_AUTOLABEL_NONE)
      return;

   char rid_copy[24];
   snprintf(rid_copy, sizeof(rid_copy), "%s", g_last_record_id);
   /* Clear before apply so a re-entrant call can't double-label. */
   g_last_record_id[0] = '\0';
   g_last_record_month[0] = '\0';
   (void)dogfood_autolabel_apply(&cfg, rid_copy, kind);
}

int dogfood_query_is_repeat(const dogfood_config_t *cfg, const char *session_id, const char *tool,
                            const char *query_hash)
{
   if (!cfg || !session_id || !tool || !query_hash || !query_hash[0] || !cfg->log_dir[0])
      return 0;

   char month[8];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(month, sizeof(month), "%Y-%m", &tm_buf);

   char path[640];
   snprintf(path, sizeof(path), "%s/%s.jsonl", cfg->log_dir, month);

   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;

   int hit = 0;
   char *line = NULL;
   size_t cap = 0;
   ssize_t n;
   while ((n = dogfood_read_line(&line, &cap, fp)) > 0)
   {
      while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
         line[--n] = '\0';
      if (n == 0)
         continue;
      cJSON *rec = cJSON_Parse(line);
      if (!rec)
         continue;
      const cJSON *sid = cJSON_GetObjectItemCaseSensitive(rec, "session_id");
      const cJSON *tn = cJSON_GetObjectItemCaseSensitive(rec, "tool");
      const cJSON *qh = cJSON_GetObjectItemCaseSensitive(rec, "query_hash");
      if (cJSON_IsString(sid) && cJSON_IsString(tn) && cJSON_IsString(qh) &&
          strcmp(sid->valuestring, session_id) == 0 && strcmp(tn->valuestring, tool) == 0 &&
          strcmp(qh->valuestring, query_hash) == 0)
      {
         hit = 1;
         cJSON_Delete(rec);
         break;
      }
      cJSON_Delete(rec);
   }
   free(line);
   fclose(fp);
   return hit;
}

int dogfood_label_record(const dogfood_config_t *cfg_in, const char *record_id,
                         const dogfood_label_t *label)
{
   if (!record_id || !record_id[0] || !label)
      return -1;

   dogfood_config_t live;
   const dogfood_config_t *cfg = cfg_in;
   if (!cfg)
   {
      dogfood_config_current(&live);
      cfg = &live;
   }
   if (!cfg->enabled || !cfg->log_dir[0])
   {
      g_failures++;
      return -1;
   }
   if (platform_mkdir_p(cfg->log_dir, 0700) != 0)
   {
      g_failures++;
      return -1;
   }

   char ts[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   char month[8];
   strftime(month, sizeof(month), "%Y-%m", &tm_buf);

   char path[640];
   snprintf(path, sizeof(path), "%s/%s.labels.jsonl", cfg->log_dir, month);

   cJSON *rec = cJSON_CreateObject();
   if (!rec)
   {
      g_failures++;
      return -1;
   }
   cJSON_AddStringToObject(rec, "ts", ts);
   cJSON_AddStringToObject(rec, "id", record_id);
   if (label->outcome && label->outcome[0])
      cJSON_AddStringToObject(rec, "outcome", label->outcome);
   if (label->context_richness > 0 && label->context_richness <= 5)
      cJSON_AddNumberToObject(rec, "context_richness", label->context_richness);
   if (label->has_surprise)
      cJSON_AddBoolToObject(rec, "surprise", label->surprise ? 1 : 0);
   if (label->prospective_surfaced)
      cJSON_AddBoolToObject(rec, "prospective_surfaced", 1);
   if (label->notes && label->notes[0])
      cJSON_AddStringToObject(rec, "notes", label->notes);

   char *line = cJSON_PrintUnformatted(rec);
   cJSON_Delete(rec);
   if (!line)
   {
      g_failures++;
      return -1;
   }

   FILE *fp = fopen(path, "a");
   if (!fp)
   {
      free(line);
      g_failures++;
      return -1;
   }
   int rc = fprintf(fp, "%s\n", line) < 0 ? -1 : 0;
   fclose(fp);
   free(line);
   if (rc != 0)
      g_failures++;
   return rc;
}

cJSON *dogfood_build_report_for_month(const char *dir_override, const char *month_override)
{
   char dir[512];
   if (dir_override && dir_override[0])
      snprintf(dir, sizeof(dir), "%s", dir_override);
   else
   {
      char log_dir[CONFIG_COPY_MAX];
      config_dogfood_log_dir_copy(log_dir, sizeof(log_dir));
      if (log_dir[0])
         snprintf(dir, sizeof(dir), "%s", log_dir);
      else
         snprintf(dir, sizeof(dir), "%s/dogfood", config_output_dir());
   }
   char month[16];
   if (month_override && month_override[0])
      snprintf(month, sizeof(month), "%s", month_override);
   else
   {
      time_t now = time(NULL);
      struct tm tm_buf;
      gmtime_r(&now, &tm_buf);
      strftime(month, sizeof(month), "%Y-%m", &tm_buf);
   }
   cJSON *records = dogfood_read_month(dir, month);
   if (!records)
      return NULL;
   cJSON *report = dogfood_build_report(records);
   cJSON_Delete(records);
   if (!report)
      return NULL;
   cJSON_AddStringToObject(report, "month", month);
   cJSON_AddStringToObject(report, "log_dir", dir);
   {
      report_subject_t subject;
      if (report_subject_from_project_root(NULL, &subject) == 0)
         (void)report_subject_add_json(report, &subject);
   }
   return report;
}
