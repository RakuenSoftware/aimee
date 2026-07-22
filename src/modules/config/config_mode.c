/* config_mode.c: operating-mode (engineer/novel) resolution and persistence.
 *
 * Kept out of config.c (at its size cap) and out of config.json: the durable
 * mode is a tiny dedicated file (<config dir>/mode) read by both the client and
 * the server. The ephemeral /novel toggle overrides it via the AIMEE_MODE
 * environment variable. Self-contained (no prompts.o dependency) so config_*
 * test targets keep linking cleanly. */
#include "config.h"
#include "prompts.h" /* aimee_mode_t */
#include <stdio.h>
#include <string.h>
#include <strings.h>

static aimee_mode_t mode_parse(const char *s)
{
   if (s && strcasecmp(s, "novel") == 0)
      return AIMEE_MODE_NOVEL;
   if (s && strcasecmp(s, "songwriter") == 0)
      return AIMEE_MODE_SONGWRITER;
   if (s && strcasecmp(s, "qa") == 0)
      return AIMEE_MODE_QA;
   if (s && strcasecmp(s, "security") == 0)
      return AIMEE_MODE_SECURITY;
   if (s && strcasecmp(s, "reviewer") == 0)
      return AIMEE_MODE_REVIEWER;
   if (s && strcasecmp(s, "architect") == 0)
      return AIMEE_MODE_ARCHITECT;
   if (s && strcasecmp(s, "reviewer-constructive") == 0)
      return AIMEE_MODE_REVIEWER_CONSTRUCTIVE;
   if (s && strcasecmp(s, "technical-writer") == 0)
      return AIMEE_MODE_TECH_WRITER;
   return AIMEE_MODE_ENGINEER;
}

static void mode_file_path(char *buf, size_t n)
{
   snprintf(buf, n, "%s/mode", config_default_dir());
}

aimee_mode_t config_current_mode(void)
{
   /* Env override wins: the propagation channel for the ephemeral /novel
    * toggle and for delegates spawned from a novel session. */
   const char *env = getenv("AIMEE_MODE");
   if (env && env[0])
      return mode_parse(env);

   char path[MAX_PATH_LEN];
   mode_file_path(path, sizeof(path));
   FILE *f = fopen(path, "r");
   if (!f)
      return AIMEE_MODE_ENGINEER;
   char buf[32] = {0};
   char *got = fgets(buf, sizeof(buf), f);
   fclose(f);
   if (!got)
      return AIMEE_MODE_ENGINEER;
   buf[strcspn(buf, "\r\n")] = '\0';
   return mode_parse(buf);
}

void config_current_persona(char *out, size_t n)
{
   if (!out || !n)
      return;
   const char *env = getenv("AIMEE_MODE");
   if (env && env[0])
   {
      snprintf(out, n, "%s", env);
      return;
   }
   char path[MAX_PATH_LEN];
   mode_file_path(path, sizeof(path));
   FILE *f = fopen(path, "r");
   if (!f)
   {
      snprintf(out, n, "engineer");
      return;
   }
   char buf[64] = {0};
   char *got = fgets(buf, sizeof(buf), f);
   fclose(f);
   if (!got || !buf[0])
   {
      snprintf(out, n, "engineer");
      return;
   }
   buf[strcspn(buf, "\r\n")] = '\0';
   snprintf(out, n, "%s", buf[0] ? buf : "engineer");
}

int config_persist_mode(const char *mode)
{
   char path[MAX_PATH_LEN];
   mode_file_path(path, sizeof(path));
   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fprintf(f, "%s\n", (mode && mode[0]) ? mode : "engineer");
   fclose(f);
   return 0;
}
