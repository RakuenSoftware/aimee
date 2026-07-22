/* delegate_backend.c: see delegate_backend.h.
 *
 * Process-global registry. Populated at server start; lookups are
 * lock-free reads of a fixed-size array. */

#include "delegate_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static delegate_backend_t *g_registry[DELEGATE_BACKEND_MAX];
static int g_count;

int delegate_backend_register(delegate_backend_t *b)
{
   if (!b || !b->name || !b->name[0])
      return -1;
   if (g_count >= DELEGATE_BACKEND_MAX)
      return -1;
   for (int i = 0; i < g_count; i++)
   {
      if (g_registry[i]->name && strcmp(g_registry[i]->name, b->name) == 0)
         return -1;
   }
   g_registry[g_count++] = b;
   return 0;
}

delegate_backend_t *delegate_backend_lookup(const char *name)
{
   if (!name || !name[0])
      return NULL;
   for (int i = 0; i < g_count; i++)
   {
      if (g_registry[i]->name && strcmp(g_registry[i]->name, name) == 0)
         return g_registry[i];
   }
   return NULL;
}

int delegate_backend_list(delegate_backend_t **out, int max)
{
   if (!out || max <= 0)
      return 0;
   int n = g_count < max ? g_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_registry[i];
   return n;
}

void delegate_backend_reset_for_test(void)
{
   for (int i = 0; i < g_count; i++)
      g_registry[i] = NULL;
   g_count = 0;
}

/* Append `s` to *buf at *off, doubling *cap as needed. JSON-escapes
 * a minimal set of characters (", \\, control chars to \uXXXX). */
static int json_append_str(char **buf, size_t *cap, size_t *off, const char *s)
{
   if (!s)
      s = "";
   for (const char *p = s; *p; p++)
   {
      /* Worst case per char is 6 bytes (\uXXXX); keep 8 of headroom. */
      if (*off + 8 >= *cap)
      {
         size_t new_cap = *cap * 2;
         char *nb = realloc(*buf, new_cap);
         if (!nb)
            return -1;
         *buf = nb;
         *cap = new_cap;
      }
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\')
      {
         (*buf)[(*off)++] = '\\';
         (*buf)[(*off)++] = (char)c;
      }
      else if (c < 0x20)
      {
         *off += (size_t)snprintf(*buf + *off, *cap - *off, "\\u%04x", c);
      }
      else
      {
         (*buf)[(*off)++] = (char)c;
      }
   }
   return 0;
}

char *delegate_backend_list_json(void)
{
   size_t cap = 256;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t off = 0;
   buf[off++] = '[';

   for (int i = 0; i < g_count; i++)
   {
      delegate_backend_t *b = g_registry[i];
      if (!b)
         continue;
      /* Reserve room for the entry skeleton — string body grows via
       * json_append_str. */
      if (off + 64 >= cap)
      {
         size_t new_cap = cap * 2;
         char *nb = realloc(buf, new_cap);
         if (!nb)
         {
            free(buf);
            return NULL;
         }
         buf = nb;
         cap = new_cap;
      }
      if (i > 0)
         buf[off++] = ',';
      memcpy(buf + off, "{\"name\":\"", 9);
      off += 9;
      if (json_append_str(&buf, &cap, &off, b->name) != 0)
      {
         free(buf);
         return NULL;
      }
      memcpy(buf + off, "\",\"description\":\"", 17);
      off += 17;
      if (json_append_str(&buf, &cap, &off, b->description) != 0)
      {
         free(buf);
         return NULL;
      }
      memcpy(buf + off, "\"}", 2);
      off += 2;
   }

   if (off + 2 >= cap)
   {
      char *nb = realloc(buf, off + 2);
      if (!nb)
      {
         free(buf);
         return NULL;
      }
      buf = nb;
      cap = off + 2;
   }
   buf[off++] = ']';
   buf[off] = '\0';
   return buf;
}

int delegate_backend_config_parse(int argc, char **argv, delegate_backend_config_t *cfg,
                                  const char **out_backend_name, int *first_non_flag_idx)
{
   if (!cfg || argc < 0 || (argc > 0 && !argv))
      return -1;
   int i = 0;
   while (i < argc)
   {
      const char *a = argv[i];
      if (!a)
         break;
      if (strcmp(a, "--backend") == 0)
      {
         if (i + 1 >= argc || !argv[i + 1])
            return -1;
         if (out_backend_name)
            *out_backend_name = argv[i + 1];
         i += 2;
         continue;
      }
      if (strcmp(a, "--image") == 0)
      {
         if (i + 1 >= argc || !argv[i + 1])
            return -1;
         cfg->image = argv[i + 1];
         i += 2;
         continue;
      }
      if (strcmp(a, "--host") == 0)
      {
         if (i + 1 >= argc || !argv[i + 1])
            return -1;
         cfg->host = argv[i + 1];
         i += 2;
         continue;
      }
      if (strcmp(a, "--no-hibernate") == 0)
      {
         cfg->hibernate_on_exit = 0;
         i += 1;
         continue;
      }
      /* Unrecognised — treat as the first non-flag arg. The caller
       * resumes scanning here for prompt / role / etc. */
      break;
   }
   if (first_non_flag_idx)
      *first_non_flag_idx = i;
   return 0;
}

int delegate_backend_wrap_command(const char *snap_path, const char *cwd_file_path,
                                  const char *user_command, char **out_script)
{
   if (out_script)
      *out_script = NULL;
   if (!snap_path || !snap_path[0] || !cwd_file_path || !cwd_file_path[0] || !user_command ||
       !out_script)
      return -1;

   /* Boilerplate adds ~250 bytes plus three path copies and the user
    * command. Compute a generous upper bound and allocate once. */
   size_t need = strlen(snap_path) * 4 + strlen(cwd_file_path) * 4 + strlen(user_command) + 512;
   char *buf = malloc(need);
   if (!buf)
      return -1;

   /* User command runs in the parent shell (not a subshell) so a
    * `cd` inside it persists into the wrapper's pwd capture below.
    * Exit code is preserved via `EXIT=$?` immediately after, so the
    * snapshot-write trailer cannot mask a failure. */
   int n = snprintf(buf, need,
                    "SNAP=%s\n"
                    "CWD_FILE=%s\n"
                    "[ -f \"$SNAP\" ] && source \"$SNAP\"\n"
                    "[ -f \"$CWD_FILE\" ] && cd \"$(cat \"$CWD_FILE\")\"\n"
                    "%s\n"
                    "EXIT=$?\n"
                    "( export -p; declare -f; alias -p ) > \"$SNAP.tmp\" 2>/dev/null\n"
                    "mv \"$SNAP.tmp\" \"$SNAP\"\n"
                    "pwd > \"$CWD_FILE.tmp\"\n"
                    "mv \"$CWD_FILE.tmp\" \"$CWD_FILE\"\n"
                    "exit $EXIT\n",
                    snap_path, cwd_file_path, user_command);
   if (n < 0 || (size_t)n >= need)
   {
      free(buf);
      return -1;
   }
   *out_script = buf;
   return 0;
}
