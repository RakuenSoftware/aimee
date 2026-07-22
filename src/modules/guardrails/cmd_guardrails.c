/* cmd_guardrails.c: aimee guardrails — semantic guardrail event review. */
#include "aimee.h"
#include "commands.h"
#include "db1.h"
#include "db1/guardrail_events.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
   fprintf(stderr, "Usage: aimee guardrails review [options]\n");
   fprintf(stderr, "  --limit N   Number of events to show (default 20)\n");
   fprintf(stderr, "  --json      Output JSON array\n");
   fprintf(stderr, "  --all       Include non-advisory events (dry-run only)\n");
}

static void print_event_json(const guardrail_event_row_t *r)
{
   printf("{\"id\":%d,\"recorded_at\":\"%s\",\"session_id\":\"%s\",\"tool_name\":\"%s\","
          "\"overall_risk\":%.3f,\"labels\":\"%s\",\"final_action\":\"%s\","
          "\"explanation\":\"%s\",\"dry_run\":%d}\n",
          r->id, r->recorded_at, r->session_id, r->tool_name, r->overall_risk, r->labels,
          r->final_action, r->explanation, r->dry_run);
}

static void print_event_interactive(const guardrail_event_row_t *r, int idx, int total)
{
   fprintf(stderr, "--- Event %d/%d ---\n", idx + 1, total);
   fprintf(stderr, "  ID:         %d\n", r->id);
   fprintf(stderr, "  Time:       %s\n", r->recorded_at);
   fprintf(stderr, "  Session:    %.16s%s\n", r->session_id,
           strlen(r->session_id) > 16 ? "..." : "");
   fprintf(stderr, "  Tool:       %s\n", r->tool_name);
   fprintf(stderr, "  Risk:       %.3f\n", r->overall_risk);
   fprintf(stderr, "  Action:     %s%s\n", r->final_action, r->dry_run ? " (dry-run)" : "");
   fprintf(stderr, "  Labels:     %s\n", r->labels[0] ? r->labels : "(none)");
   fprintf(stderr, "  Explanation: %.80s%s\n", r->explanation,
           strlen(r->explanation) > 80 ? "..." : "");
   fprintf(stderr, "\n");
}

static void interactive_pager(int total)
{
   if (total == 0)
      return;
   fprintf(stderr, "[n]ext  [q]uit\n");
}

void cmd_guardrails(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   int limit = 20;
   int json_mode = 0;
   int only_advisory = 1;
   int show_all = 0;

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
         limit = atoi(argv[++i]);
      else if (strcmp(argv[i], "--json") == 0)
         json_mode = 1;
      else if (strcmp(argv[i], "--all") == 0)
         show_all = 1;
      else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
      {
         usage();
         return;
      }
   }

   const char *subcmd = argc > 0 ? argv[0] : NULL;
   if (!subcmd || strcmp(subcmd, "review") != 0)
   {
      if (subcmd)
         fprintf(stderr, "Unknown guardrails subcommand: %s\n", subcmd);
      usage();
      return;
   }

   only_advisory = show_all ? 0 : 1;

   config_t cfg;
   config_load(&cfg);

   if (db1_init(cfg.db1_path) != 0)
   {
      fprintf(stderr, "guardrails: cannot open DB1 at %s\n", cfg.db1_path);
      return;
   }

   int capacity = limit;
   guardrail_event_row_t *rows = calloc((size_t)capacity, sizeof(guardrail_event_row_t));
   if (!rows)
   {
      fprintf(stderr, "guardrails: out of memory\n");
      return;
   }

   int count = 0;
   if (db1_guardrail_event_list(limit, only_advisory, rows, &count) != 0)
   {
      fprintf(stderr, "guardrails: query failed\n");
      free(rows);
      return;
   }

   if (json_mode)
   {
      printf("[");
      for (int i = 0; i < count; i++)
      {
         if (i > 0)
            printf(",");
         print_event_json(&rows[i]);
      }
      printf("]\n");
   }
   else
   {
      if (count == 0)
      {
         fprintf(stderr, "No guardrail events found.\n");
         free(rows);
         return;
      }
      for (int i = 0; i < count; i++)
      {
         print_event_interactive(&rows[i], i, count);
         if (i < count - 1)
            interactive_pager(count);
      }
   }

   free(rows);
}