/* cmd_rewind.c: aimee rewind — file-snapshot checkpoint list/restore. */
#include "aimee.h"
#include "db1.h"
#include "commands.h"
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rewind_print_help(void)
{
   fprintf(stderr,
           "Usage: aimee rewind <subcommand> [args]\n"
           "\n"
           "Subcommands:\n"
           "  list                          List checkpoints for the current session\n"
           "  create [label]                Create an empty checkpoint at the current turn\n"
           "  add <cp_id> <path>            Record `path` into an existing checkpoint\n"
           "  restore <cp_id>               Restore files recorded in checkpoint <cp_id>\n"
           "  restore <cp_id> --no-restore  Truncate conversation only, skip file restore\n"
           "  prune [keep]                  Prune checkpoints, keeping N newest (default 50)\n"
           "\n"
           "Session is resolved from the AIMEE_SESSION_ID environment variable.\n");
}

static void cmd_rewind_list(app_ctx_t *ctx)
{
   if (!db1_store_ready())
      fatal("rewind list: could not initialize DB1");
   const char *sid = session_id();
   if (!sid || !sid[0])
   {
      fprintf(stderr, "rewind: no active session (set AIMEE_SESSION_ID)\n");
      return;
   }
   fsnap_info_t rows[64];
   int n = db1_fsnap_list(sid, rows, 64);
   if (n < 0)
   {
      fprintf(stderr, "rewind: list failed\n");
      return;
   }
   if (n == 0)
   {
      printf("No checkpoints for session %s.\n", sid);
      return;
   }
   printf("%-8s  %-6s  %-19s  %-5s  %s\n", "ID", "TURN", "CREATED", "FILES", "LABEL");
   for (int i = 0; i < n; i++)
   {
      printf("%-8lld  %-6d  %-19s  %-5d  %s\n", (long long)rows[i].id, rows[i].turn,
             rows[i].created_at, rows[i].file_count, rows[i].label);
   }
}

static void cmd_rewind_create(app_ctx_t *ctx, int argc, char **argv)
{
   if (!db1_store_ready())
      fatal("rewind create: could not initialize DB1");
   const char *sid = session_id();
   if (!sid || !sid[0])
   {
      fprintf(stderr, "rewind: no active session (set AIMEE_SESSION_ID)\n");
      return;
   }
   const char *label = (argc > 0) ? argv[0] : "";
   int64_t id = db1_fsnap_create(sid, 0, label);
   if (id < 0)
      fprintf(stderr, "rewind: create failed\n");
   else
      printf("Created checkpoint %lld\n", (long long)id);
}

static void cmd_rewind_add(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "rewind add: usage: aimee rewind add <cp_id> <path>\n");
      return;
   }
   if (!db1_store_ready())
      fatal("rewind add: could not initialize DB1");
   int64_t cp_id = (int64_t)atoll(argv[0]);
   int rc = db1_fsnap_record_file(cp_id, argv[1]);
   if (rc != 0)
      fprintf(stderr, "rewind: record failed for %s\n", argv[1]);
   else
      printf("Recorded %s into checkpoint %lld\n", argv[1], (long long)cp_id);
}

static void cmd_rewind_restore(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "rewind restore: usage: aimee rewind restore <cp_id> [--no-restore]\n");
      return;
   }

   /* Parse --no-restore flag */
   int no_restore = 0;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--no-restore") == 0)
         no_restore = 1;
   }
   if (!db1_store_ready())
      fatal("rewind restore: could not initialize DB1");
   int64_t cp_id = (int64_t)atoll(argv[0]);
   fsnap_info_t info;
   if (db1_fsnap_get(cp_id, &info) != 0)
   {
      fprintf(stderr, "rewind: checkpoint %lld not found\n", (long long)cp_id);
      return;
   }

   if (no_restore)
   {
      /* Truncate conversation windows after checkpoint turn; skip file restore */
      const char *sid = info.session_id[0] ? info.session_id : session_id();
      int truncated = 0;
      if (sid && sid[0])
         truncated = db1_windows_delete_after_turn(sid, info.turn);
      if (truncated < 0)
         fprintf(stderr, "rewind: conversation truncation failed\n");
      else
         printf("Checkpoint %lld: conversation truncated (%d window(s) removed after turn %d)\n",
                (long long)cp_id, truncated, info.turn);
   }
   else
   {
      int restored = 0, deleted = 0;
      int rc = db1_fsnap_restore(cp_id, &restored, &deleted);
      printf("Checkpoint %lld: %d restored, %d deleted%s\n", (long long)cp_id, restored, deleted,
             rc == 0 ? "" : " (with errors)");
   }
}

static void cmd_rewind_prune(app_ctx_t *ctx, int argc, char **argv)
{
   if (!db1_store_ready())
      fatal("rewind prune: could not initialize DB1");
   const char *sid = session_id();
   if (!sid || !sid[0])
   {
      fprintf(stderr, "rewind: no active session (set AIMEE_SESSION_ID)\n");
      return;
   }
   int keep = FSNAP_MAX_PER_SESSION_DEFAULT;
   if (argc > 0)
      keep = atoi(argv[0]);
   int pruned = db1_fsnap_prune(sid, keep);
   if (pruned < 0)
      fprintf(stderr, "rewind: prune failed\n");
   else
      printf("Pruned %d checkpoint(s), keeping %d newest.\n", pruned, keep);
}

void cmd_rewind(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1 || strcmp(argv[0], "help") == 0 || strcmp(argv[0], "--help") == 0 ||
       strcmp(argv[0], "-h") == 0)
   {
      rewind_print_help();
      return;
   }
   if (strcmp(argv[0], "list") == 0)
      cmd_rewind_list(ctx);
   else if (strcmp(argv[0], "create") == 0)
      cmd_rewind_create(ctx, argc - 1, argv + 1);
   else if (strcmp(argv[0], "add") == 0)
      cmd_rewind_add(ctx, argc - 1, argv + 1);
   else if (strcmp(argv[0], "restore") == 0)
      cmd_rewind_restore(ctx, argc - 1, argv + 1);
   else if (strcmp(argv[0], "prune") == 0)
      cmd_rewind_prune(ctx, argc - 1, argv + 1);
   else
   {
      fprintf(stderr, "rewind: unknown subcommand '%s'\n", argv[0]);
      rewind_print_help();
   }
}
