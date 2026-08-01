#include "kb_curator_grounding.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <stdio.h>

static const char *const side_effecting_funcs[] = {
    "accept",       "aimee_pg_exec", "aimee_pg_step", "bind",
    "chdir",        "chmod",         "chown",         "close",
    "creat",        "execle",        "execl",         "execlp",
    "execv",        "execve",        "execvp",        "fclose",
    "fgets",        "fopen",         "fputs",         "fread",
    "freopen",      "fsync",         "fdatasync",     "fdopen",
    "fflush",       "ftruncate",     "fwrite",        "fprintf",
    "fork",         "ioctl",         "kill",          "link",
    "listen",       "lseek",         "mkdir",         "mmap",
    "munmap",       "open",          "openat",        "pclose",
    "popen",        "posix_spawn",   "pread",         "PQexec",
    "PQexecParams", "putenv",        "pwrite",        "raise",
    "read",         "recv",          "recvfrom",      "remove",
    "rename",       "renameat",      "rewind",        "rmdir",
    "send",         "sendto",        "setenv",        "sigaction",
    "signal",       "socket",        "sqlite3_exec",  "sqlite3_prepare_v2",
    "sqlite3_step", "symlink",       "system",        "truncate",
    "unlink",       "unlinkat",      "unsetenv",      "vfprintf",
    "vfork",        "write",
};

static const size_t side_effecting_funcs_count =
    sizeof(side_effecting_funcs) / sizeof(side_effecting_funcs[0]);

int kb_curator_callee_is_side_effecting(const char *callee)
{
   if (!callee || !callee[0])
   {
      return 0;
   }
   /* Linear scan rather than bsearch: the table is tiny (looked up once per
    * code unit) and a linear scan is immune to ordering / case-sort mistakes
    * when the table is hand-edited (e.g. uppercase "PQexec" sorts before all
    * lowercase entries under strcmp, which silently breaks a binary search). */
   for (size_t i = 0; i < side_effecting_funcs_count; i++)
   {
      if (strcmp(callee, side_effecting_funcs[i]) == 0)
      {
         return 1;
      }
   }
   return 0;
}

static int is_none_like(const char *s)
{
   static const char *const none_like[] = {
       "none", "no", "no side effects", "pure", "n/a",
   };
   static const size_t count = sizeof(none_like) / sizeof(none_like[0]);
   size_t i;
   if (!s)
   {
      return 0;
   }
   for (i = 0; i < count; i++)
   {
      if (strcasecmp(s, none_like[i]) == 0)
      {
         return 1;
      }
   }
   return 0;
}

int kb_curator_payload_claims_no_side_effects(const struct cJSON *payload)
{
   if (!payload)
   {
      return 1;
   }
   const cJSON *se = cJSON_GetObjectItemCaseSensitive(payload, "side_effects");
   if (!se)
   {
      return 1;
   }
   if (cJSON_IsNull(se))
   {
      return 1;
   }
   if (cJSON_IsArray(se))
   {
      if (cJSON_GetArraySize(se) == 0)
      {
         return 1;
      }
      cJSON *item;
      cJSON_ArrayForEach(item, se)
      {
         if (!cJSON_IsString(item))
         {
            return 0;
         }
         if (!is_none_like(item->valuestring))
         {
            return 0;
         }
      }
      return 1;
   }
   if (cJSON_IsString(se))
   {
      return is_none_like(se->valuestring) ? 1 : 0;
   }
   return 0;
}

int kb_curator_grounding_contradicts(const struct cJSON *payload, const char *const *callees,
                                     int n_callees, char *reason_out, size_t reason_len)
{
   if (reason_out && reason_len > 0)
   {
      reason_out[0] = '\0';
   }
   if (!kb_curator_payload_claims_no_side_effects(payload))
   {
      return 0;
   }
   for (int i = 0; i < n_callees; i++)
   {
      if (kb_curator_callee_is_side_effecting(callees[i]))
      {
         if (reason_out && reason_len > 0)
         {
            snprintf(reason_out, reason_len, "%s", callees[i]);
         }
         return 1;
      }
   }
   return 0;
}