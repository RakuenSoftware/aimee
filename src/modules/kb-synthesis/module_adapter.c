#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/kb-synthesis/module_api.h>

#include <string.h>

/* This C handler is a process-parity fixture only. Production aimee-kb calls
 * the separately supervised Go handler through obs_bus_module_call. */
static const char *const side_effecting_callees[] = {
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

static int ascii_equal_fold(const char *value, const char *expected)
{
   if (!value || !expected || strlen(value) != strlen(expected))
      return 0;
   for (; *value; ++value, ++expected)
   {
      unsigned char left = (unsigned char)*value;
      if (left >= 'A' && left <= 'Z')
         left = (unsigned char)(left + ('a' - 'A'));
      if (left != (unsigned char)*expected)
         return 0;
   }
   return 1;
}

static int none_like(const char *value)
{
   static const char *const values[] = {"none", "no", "no side effects", "pure", "n/a"};
   for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
      if (ascii_equal_fold(value, values[i]))
         return 1;
   return 0;
}

static int claims_no_side_effects(const aimee_kb_synthesis_grounding_request_t *request)
{
   if (request->claim_kind == AIMEE_KB_SYNTHESIS_CLAIM_NONE)
      return 1;
   if (request->claim_kind == AIMEE_KB_SYNTHESIS_CLAIM_STRING)
      return none_like(request->claims[0]);
   if (request->claim_kind != AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY)
      return 0;
   for (uint32_t i = 0; i < request->claim_count; ++i)
      if (!none_like(request->claims[i]))
         return 0;
   return 1;
}

static int callee_side_effecting(const char *callee)
{
   for (size_t i = 0; i < sizeof(side_effecting_callees) / sizeof(side_effecting_callees[0]); ++i)
      if (strcmp(callee, side_effecting_callees[i]) == 0)
         return 1;
   return 0;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   aimee_kb_synthesis_grounding_request_t request;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_KB_SYNTHESIS_STAGE_GROUNDING ||
       response_capacity < AIMEE_KB_SYNTHESIS_RESPONSE_LEN ||
       aimee_kb_synthesis_request_decode(request_body, request_len, &request) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   int contradicts = 0;
   const char *reason = "";
   if (claims_no_side_effects(&request))
      for (uint32_t i = 0; i < request.callee_count; ++i)
         if (callee_side_effecting(request.callees[i]))
         {
            contradicts = 1;
            reason = request.callees[i];
            break;
         }
   if (aimee_kb_synthesis_response_encode(contradicts, reason, response_body, response_capacity) !=
       0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_KB_SYNTHESIS_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
