/* cmd_dogfood.c: dogfood qualitative log surfaces.
 *
 *   aimee dogfood tag <record-id> [--outcome X] [--richness N]
 *                                  [--surprise|--no-surprise] [--notes T]
 *   aimee dogfood review [--month YYYY-MM] [--limit N]
 *   aimee dogfood report [--month YYYY-MM] [--json]
 *
 * Records live in <dogfood_log_dir>/YYYY-MM.jsonl and labels in the
 * sibling YYYY-MM.labels.jsonl. The report joins the two by record id.
 */

#include "aimee.h"
#include "commands.h"
#include "dogfood.h"
#include "cJSON.h"
#include "kb_client.h"
#include "platform_path.h"
#include "report_enrichment.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* --- shared helpers --- */

static const char *KNOWN_OUTCOMES[] = {"hit", "partial", "miss", "hallucination", NULL};

static int outcome_is_valid(const char *s)
{
   if (!s || !s[0])
      return 0;
   for (int i = 0; KNOWN_OUTCOMES[i]; i++)
      if (strcmp(s, KNOWN_OUTCOMES[i]) == 0)
         return 1;
   return 0;
}

static void resolve_log_dir(char *out, size_t cap)
{
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.dogfood_log_dir[0])
      snprintf(out, cap, "%s", cfg.dogfood_log_dir);
   else
      snprintf(out, cap, "%s/dogfood", config_output_dir());
}

static void resolve_month(char *out, size_t cap, const char *override)
{
   if (override && override[0])
   {
      snprintf(out, cap, "%s", override);
      return;
   }
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(out, cap, "%Y-%m", &tm_buf);
}

/* Forward declarations for the prospective-memory helpers so review
 * can complete reminders without reordering the source. */
static int arm_review_reminder_if_needed(const char *month, int unlabelled);
static int complete_review_reminders(void);

/* --- tag subcommand --- */

static void dogfood_sub_tag(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("dogfood tag requires <record-id>");
   const char *record_id = argv[0];

   dogfood_label_t label = {0};
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--outcome") == 0 && i + 1 < argc)
      {
         if (!outcome_is_valid(argv[++i]))
            fatal("--outcome must be one of hit/partial/miss/hallucination");
         label.outcome = argv[i];
      }
      else if (strcmp(argv[i], "--richness") == 0 && i + 1 < argc)
      {
         int r = atoi(argv[++i]);
         if (r < 1 || r > 5)
            fatal("--richness must be 1..5");
         label.context_richness = r;
      }
      else if (strcmp(argv[i], "--surprise") == 0)
      {
         label.surprise = 1;
         label.has_surprise = 1;
      }
      else if (strcmp(argv[i], "--no-surprise") == 0)
      {
         label.surprise = 0;
         label.has_surprise = 1;
      }
      else if (strcmp(argv[i], "--notes") == 0 && i + 1 < argc)
      {
         label.notes = argv[++i];
      }
      else
      {
         fatal("unknown dogfood tag flag: %s", argv[i]);
      }
   }

   int rc = dogfood_label_record(NULL, record_id, &label);
   if (rc != 0)
      fatal("dogfood tag: failed to write label (is dogfood_enabled=true?)");
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("tagged %s\n", record_id);
}

/* --- review subcommand --- */

static int review_prompt_char(const char *prompt, int default_yes)
{
   fputs(prompt, stdout);
   fflush(stdout);
   int c = getchar();
   if (c == '\n' || c == EOF)
      return default_yes;
   /* Consume the rest of the line. */
   int ch;
   while ((ch = getchar()) != '\n' && ch != EOF)
      ;
   return c;
}

static void dogfood_sub_review(app_ctx_t *ctx, int argc, char **argv)
{
   const char *month_override = NULL;
   int limit = 20;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--month") == 0 && i + 1 < argc)
         month_override = argv[++i];
      else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
         limit = atoi(argv[++i]);
      else
         fatal("unknown dogfood review flag: %s", argv[i]);
   }

   char dir[512], month[16];
   resolve_log_dir(dir, sizeof(dir));
   resolve_month(month, sizeof(month), month_override);

   cJSON *records = dogfood_read_month(dir, month);
   if (!records)
      fatal("dogfood review: no log for %s at %s", month, dir);

   int reviewed = 0;
   int total = cJSON_GetArraySize(records);
   if (total == 0)
   {
      printf("No records for %s.\n", month);
      cJSON_Delete(records);
      return;
   }

   cJSON *rec;
   int idx = 0;
   cJSON_ArrayForEach(rec, records)
   {
      idx++;
      if (cJSON_GetObjectItemCaseSensitive(rec, "outcome"))
         continue; /* already labelled */
      if (reviewed >= limit)
         break;
      cJSON *id = cJSON_GetObjectItemCaseSensitive(rec, "id");
      cJSON *tool = cJSON_GetObjectItemCaseSensitive(rec, "tool");
      cJSON *ts = cJSON_GetObjectItemCaseSensitive(rec, "ts");
      cJSON *rcount = cJSON_GetObjectItemCaseSensitive(rec, "retrieved_count");
      cJSON *q = cJSON_GetObjectItemCaseSensitive(rec, "query");
      cJSON *qh = cJSON_GetObjectItemCaseSensitive(rec, "query_hash");

      printf("[%d/%d] %s  tool=%s  retrieved=%d\n", idx, total,
             cJSON_IsString(ts) ? ts->valuestring : "",
             cJSON_IsString(tool) ? tool->valuestring : "",
             cJSON_IsNumber(rcount) ? rcount->valueint : 0);
      if (cJSON_IsString(q))
         printf("  query: %s\n", q->valuestring);
      else if (cJSON_IsString(qh))
         printf("  query_hash: %s\n", qh->valuestring);
      printf("  id: %s\n", cJSON_IsString(id) ? id->valuestring : "");

      int outcome_c =
          review_prompt_char("  outcome [h]it/[p]artial/[m]iss/[x]hallucination/[s]kip: ", 's');
      const char *outcome = NULL;
      switch (outcome_c)
      {
      case 'h':
      case 'H':
         outcome = "hit";
         break;
      case 'p':
      case 'P':
         outcome = "partial";
         break;
      case 'm':
      case 'M':
         outcome = "miss";
         break;
      case 'x':
      case 'X':
         outcome = "hallucination";
         break;
      default:
         outcome = NULL;
         break;
      }
      if (!outcome)
      {
         printf("  skipped.\n");
         continue;
      }
      dogfood_label_t label = {0};
      label.outcome = outcome;
      /* Optional surprise tag for hits. */
      if (strcmp(outcome, "hit") == 0)
      {
         int s = review_prompt_char("  surprise? [y]/[N]: ", 'n');
         if (s == 'y' || s == 'Y')
         {
            label.surprise = 1;
            label.has_surprise = 1;
         }
      }
      if (cJSON_IsString(id))
      {
         if (dogfood_label_record(NULL, id->valuestring, &label) == 0)
            reviewed++;
      }
   }

   cJSON_Delete(records);

   /* Close any armed review reminders so the cross-session loop
    * completes cleanly. */
   int closed = complete_review_reminders();

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "reviewed", reviewed);
      cJSON_AddNumberToObject(j, "reminders_completed", closed);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("reviewed %d record(s); closed %d armed reminder(s)\n", reviewed, closed);
   }
}

/* Add 45 days to a YYYY-MM month tag (first of month) and return as
 * YYYY-MM-DD HH:MM:SS. Conservative valid_until so operators who
 * travel or take a week off still have the reminder armed when they
 * return. */
static void compute_review_deadline(const char *month, char *out, size_t cap)
{
   struct tm tm_buf;
   memset(&tm_buf, 0, sizeof(tm_buf));
   int y = 0, m = 0;
   if (sscanf(month, "%d-%d", &y, &m) != 2 || y < 1970 || m < 1 || m > 12)
   {
      if (cap > 0)
         out[0] = '\0';
      return;
   }
   tm_buf.tm_year = y - 1900;
   tm_buf.tm_mon = m; /* next month 0-indexed is this month 1-indexed */
   tm_buf.tm_mday = 15;
   tm_buf.tm_hour = 0;
   tm_buf.tm_min = 0;
   tm_buf.tm_sec = 0;
   time_t t = timegm(&tm_buf);
   struct tm out_tm;
   gmtime_r(&t, &out_tm);
   strftime(out, cap, "%Y-%m-%d %H:%M:%S", &out_tm);
}

/* Pull the "prospectives" array out of a kb response envelope.  Returns
 * NULL when status != ok or the array is missing.  Caller frees. */
static cJSON *dogfood_detach_prospectives(char *envelope)
{
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "prospectives");
   cJSON *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsArray(arr))
      out = cJSON_DetachItemViaPointer(resp, arr);
   cJSON_Delete(resp);
   return out;
}

/* Arm a prospective-memory reminder once per month so the operator's
 * next session that mentions "dogfood review" surfaces a pointer at
 * the unlabelled records. Skips if a reminder with the same trigger
 * text is already armed. Returns 1 if a reminder was armed, 0 if
 * skipped or the DB was unavailable. */
static int arm_review_reminder_if_needed(const char *month, int unlabelled)
{
   if (!month || !month[0])
      return 0;
   char trigger[96];
   snprintf(trigger, sizeof(trigger), "dogfood-review-%s", month);

   cJSON *existing = dogfood_detach_prospectives(kb_client_memory_prospective_list_json(NULL, 256));
   if (existing)
   {
      cJSON *r = NULL;
      cJSON_ArrayForEach(r, existing)
      {
         cJSON *trig = cJSON_GetObjectItemCaseSensitive(r, "trigger_text");
         if (cJSON_IsString(trig) && strcmp(trig->valuestring, trigger) == 0)
         {
            cJSON_Delete(existing);
            return 0;
         }
      }
      cJSON_Delete(existing);
   }

   char action[256];
   snprintf(action, sizeof(action), "review %d record(s) for %s: aimee dogfood review --month %s",
            unlabelled, month, month);
   char valid_until[32];
   compute_review_deadline(month, valid_until, sizeof(valid_until));

   char *envelope =
       kb_client_memory_prospective_create_json(trigger, action, "", "", "", valid_until);
   if (!envelope)
      return 0;
   cJSON *resp = cJSON_Parse(envelope);
   free(envelope);
   int armed_ok = 0;
   if (resp)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      armed_ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
      cJSON_Delete(resp);
   }
   return armed_ok ? 1 : 0;
}

/* On the inverse path: when the operator runs `aimee dogfood review`,
 * complete every armed review reminder so the loop closes cleanly. */
static int complete_review_reminders(void)
{
   cJSON *armed = dogfood_detach_prospectives(kb_client_memory_prospective_list_json("armed", 64));
   if (!armed)
      return 0;
   int closed = 0;
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, armed)
   {
      cJSON *trig = cJSON_GetObjectItemCaseSensitive(r, "trigger_text");
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
      if (!cJSON_IsString(trig) || !cJSON_IsNumber(id_j))
         continue;
      if (strncmp(trig->valuestring, "dogfood-review-", 15) != 0)
         continue;
      char *env = kb_client_memory_prospective_complete_json((int64_t)id_j->valuedouble);
      if (!env)
         continue;
      cJSON *resp = cJSON_Parse(env);
      free(env);
      if (resp)
      {
         cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
         if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
            closed++;
         cJSON_Delete(resp);
      }
   }
   cJSON_Delete(armed);
   return closed;
}

/* --- report subcommand --- */

/* Slurp a file into a heap-allocated NUL-terminated buffer. Returns
 * NULL on any error. Caller frees. */
static char *slurp_file(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (n < 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[rd] = '\0';
   return buf;
}

/* Scan every markdown file in docs/proposals/pending/. Proposals that
 * declare a `## Dogfood Signals` block get a real confirmed /
 * contradicted / no-signal classification against the merged month's
 * records; the rest stay at no-signal by default. */
static cJSON *build_proposal_cross_reference(const cJSON *records)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   DIR *d = opendir("docs/proposals/pending");
   if (!d)
      return arr;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      const char *n = ent->d_name;
      size_t nl = strlen(n);
      if (nl < 4 || strcmp(n + nl - 3, ".md") != 0)
         continue;
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "proposal", n);
      const char *status = "no-signal";
      int signal_count = 0;

      char path[512];
      snprintf(path, sizeof(path), "docs/proposals/pending/%s", n);
      char *md = slurp_file(path);
      if (md)
      {
         cJSON *signals = dogfood_parse_signals_md(md);
         if (signals)
         {
            signal_count = cJSON_GetArraySize(signals);
            status = dogfood_classify(records, signals);
         }
         cJSON_Delete(signals);
         free(md);
      }
      cJSON_AddStringToObject(row, "status", status);
      cJSON_AddNumberToObject(row, "signal_count", signal_count);
      cJSON_AddItemToArray(arr, row);
   }
   closedir(d);
   return arr;
}

static void dogfood_sub_report(app_ctx_t *ctx, int argc, char **argv)
{
   const char *month_override = NULL;
   int force_json = 0;
   const char *dir_override = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--month") == 0 && i + 1 < argc)
         month_override = argv[++i];
      else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
         dir_override = argv[++i];
      else if (strcmp(argv[i], "--json") == 0)
         force_json = 1;
      else
         fatal("unknown dogfood report flag: %s", argv[i]);
   }

   char dir[512], month[16];
   if (dir_override && dir_override[0])
      snprintf(dir, sizeof(dir), "%s", dir_override);
   else
      resolve_log_dir(dir, sizeof(dir));
   resolve_month(month, sizeof(month), month_override);

   cJSON *records = dogfood_read_month(dir, month);
   if (!records)
      fatal("dogfood report: could not read %s/%s.jsonl", dir, month);
   cJSON *report = dogfood_build_report(records);
   if (!report)
   {
      cJSON_Delete(records);
      fatal("dogfood report: could not build report");
   }
   cJSON_AddStringToObject(report, "month", month);
   cJSON_AddStringToObject(report, "log_dir", dir);
   {
      report_subject_t subject;
      if (report_subject_from_project_root(NULL, &subject) == 0)
         (void)report_subject_add_json(report, &subject);
   }
   cJSON *proposals = build_proposal_cross_reference(records);
   cJSON_Delete(records);
   if (proposals)
      cJSON_AddItemToObject(report, "proposal_cross_reference", proposals);

   /* Arm a prospective reminder the first time this month is reported. */
   {
      cJSON *rt = cJSON_GetObjectItemCaseSensitive(report, "records_total");
      cJSON *rl = cJSON_GetObjectItemCaseSensitive(report, "records_labelled");
      int unlabelled =
          (cJSON_IsNumber(rt) ? rt->valueint : 0) - (cJSON_IsNumber(rl) ? rl->valueint : 0);
      int armed = arm_review_reminder_if_needed(month, unlabelled);
      cJSON_AddBoolToObject(report, "review_reminder_armed", armed ? 1 : 0);
   }

   if (ctx->json_output || force_json)
   {
      emit_json_ctx(report, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      cJSON *rt = cJSON_GetObjectItemCaseSensitive(report, "records_total");
      cJSON *rl = cJSON_GetObjectItemCaseSensitive(report, "records_labelled");
      cJSON *ra = cJSON_GetObjectItemCaseSensitive(report, "records_auto_labelled");
      cJSON *rz = cJSON_GetObjectItemCaseSensitive(report, "records_retrieved_zero");
      printf("dogfood report for %s (%s)\n", month, dir);
      printf("  records: %d (labelled: %d, auto-labelled: %d, retrieved-zero: %d)\n",
             cJSON_IsNumber(rt) ? rt->valueint : 0, cJSON_IsNumber(rl) ? rl->valueint : 0,
             cJSON_IsNumber(ra) ? ra->valueint : 0, cJSON_IsNumber(rz) ? rz->valueint : 0);

      cJSON *al = cJSON_GetObjectItemCaseSensitive(report, "auto_labels");
      if (al && cJSON_GetArraySize(al) > 0)
      {
         printf("  auto-labels:\n");
         cJSON *entry;
         cJSON_ArrayForEach(entry, al)
         {
            printf("    %-18s %d\n", entry->string, entry->valueint);
         }
      }

      cJSON *per_tool = cJSON_GetObjectItemCaseSensitive(report, "per_tool");
      if (per_tool)
      {
         printf("  per tool:\n");
         cJSON *entry;
         cJSON_ArrayForEach(entry, per_tool)
         {
            printf("    %-18s %d\n", entry->string, entry->valueint);
         }
      }

      cJSON *outcomes = cJSON_GetObjectItemCaseSensitive(report, "outcomes");
      if (outcomes)
      {
         printf("  outcomes:\n");
         cJSON *entry;
         cJSON_ArrayForEach(entry, outcomes)
         {
            printf("    %-16s %d\n", entry->string, entry->valueint);
         }
      }

      cJSON *surprises = cJSON_GetObjectItemCaseSensitive(report, "surprise_wins");
      if (surprises && cJSON_GetArraySize(surprises) > 0)
         printf("  surprise wins: %d\n", cJSON_GetArraySize(surprises));
      cJSON *hard = cJSON_GetObjectItemCaseSensitive(report, "hard_negatives");
      if (hard && cJSON_GetArraySize(hard) > 0)
         printf("  hard negatives: %d\n", cJSON_GetArraySize(hard));
      cJSON *pros = cJSON_GetObjectItemCaseSensitive(report, "prospective_surfaced");
      if (pros && cJSON_GetArraySize(pros) > 0)
         printf("  prospective-memory surfaced: %d\n", cJSON_GetArraySize(pros));

      cJSON *xref = cJSON_GetObjectItemCaseSensitive(report, "proposal_cross_reference");
      if (xref)
      {
         int n = cJSON_GetArraySize(xref);
         printf("  proposal cross-reference (%d pending): all no-signal until richer "
                "category-labelling lands\n",
                n);
      }
      cJSON_Delete(report);
   }
}

/* --- dispatch --- */

static const subcmd_t cmd_dogfood_subs[] = {
    {"tag", "tag a record with outcome/richness", dogfood_sub_tag},
    {"review", "review a month's dogfood log", dogfood_sub_review},
    {"report", "report on a month's dogfood outcomes", dogfood_sub_report},
    {NULL, NULL, NULL},
};

void cmd_dogfood(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("dogfood requires a subcommand: tag, review, report");
   const char *sub = argv[0];
   argc--;
   argv++;
   if (subcmd_dispatch(cmd_dogfood_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown dogfood subcommand: %s", sub);
}
