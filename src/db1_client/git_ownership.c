/* db1_client/git_ownership.c: the git_ownership family, reached over the bus.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * Same functions, same contract, different side of the boundary: the daemon
 * links this instead of the DB1 domain, so nothing that calls these had to
 * change.
 *
 * It lives OUTSIDE modules/db1 deliberately. The module's descriptor owns every
 * .c beside it and compiles them into the DB1 process, so a client with these
 * names in that directory would be linked twice into the one binary that must
 * not have it -- once as the caller and once as the implementation.

 *
 * A failure returns -1, never "no owner". branch_own_check() reads a negative
 * as "no enforcement" and allows the operation, which is what it has always
 * done without a database, whereas a fabricated 0 would assert the branch is
 * unowned and let a caller take one somebody else holds. *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
#include "git_ownership.h"

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_GIT_OWNERSHIP_CALL_TIMEOUT_MS 2000

static void warn_unreachable(int reason)
{
   static int warned;
   if (warned)
      return;
   warned = 1;
   /* Said once per process: enough to tell a store that is down from one that
      is quiet, without one line per call. The numeric
      aimee_module_call_result_t, not its name, so this does not pull the whole
      event-bus library in behind the client for one string. */
   LOG_WARN("db1.git_ownership", "DB1 %s is unreachable (module call result %d)", "git ownership",
            reason);
}

/* Size the frame from the arguments themselves.

   These carry prompts, results and JSON documents, not just identifiers, and
   in-process callers have always passed them whole. A fixed cap here would
   refuse exactly those calls and return the same -1 as a broken store -- fine
   in a test with short strings, wrong the first time a real prompt arrives. The
   bus bounds the message instead. */
static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      /* Empty is legal on the wire: an optional field the caller left out
         travels as zero length. Which fields may be empty is the operation's
         business, checked before the frame is built. */
      if (!fields[i])
         return -1;
      size_t n = strlen(fields[i]);
      if (n > AIMEE_MODULE_MESSAGE_MAX_BODY - need - 4u)
         return -1;
      need += 4u + n;
   }
   *need_out = need;
   return 0;
}

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static void encode(uint8_t *out, uint32_t op, const char *const *fields, uint32_t count)
{
   uint32_t at = 0;
   aimee_db1_put_u32(out + at, op);
   at += 4u;
   aimee_db1_put_u32(out + at, count);
   at += 4u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(fields[i]);
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, fields[i], n);
      at += n;
   }
}

/* Returns the module's status, or -1 when the call never produced one. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *value_out,
                      size_t value_len)
{
   if (value_out && value_len)
      value_out[0] = '\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_GIT_OWNERSHIP))
   {
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }

   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
      return -1;
   /* The reply is bounded by the caller's own buffer: it asked for at most
      value_len bytes, so there is no reason to hold more than that. */
   size_t response_cap = 12u + (value_out ? value_len : 0u);
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   encode(request, op, fields, count);

   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_GIT_OWNERSHIP_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_GIT_OWNERSHIP, AIMEE_DB1_STAGE_GIT_OWNERSHIP, 0, deadline,
                           request, (uint32_t)request_len, response, (uint32_t)response_cap,
                           &response_len, NULL, NULL);
   free(request);

   int result = -1;
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
      warn_unreachable((int)rc);
   else
   {
      uint32_t status = aimee_db1_get_u32(response);
      uint32_t fields_in = aimee_db1_get_u32(response + 4u);
      /* Read the reply's own count rather than assuming one value: a status
         with no values is how a write answers, and a row is how a read will. */
      result = (int)status;
      if (fields_in == 0u)
      {
         if (value_out && value_len)
            value_out[0] = '\0';
      }
      else
      {
         uint32_t at = 8u;
         if (at + 4u > response_len)
            result = -1;
         else
         {
            uint32_t n = aimee_db1_get_u32(response + at);
            at += 4u;
            /* A reply whose declared length runs past what arrived is not a
               reply to read part of. */
            if (at + n > response_len)
               result = -1;
            else if (value_out && value_len)
            {
               if (n >= value_len)
                  result = -1;
               else
               {
                  memcpy(value_out, response + at, n);
                  value_out[n] = '\0';
               }
            }
         }
      }
   }
   free(response);
   return result;
}

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

/* A read answers found(1) / not-found(0) / error(-1), which is what the direct
   implementation returns and what its callers already branch on. */
static int read_result(int status, char *value_out)
{
   if (status == (int)AIMEE_DB1_STATUS_OK)
      return (value_out && value_out[0]) ? 1 : 0;
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return -1;
}

int db1_git_ownership_upsert(const char *repo_path, const char *branch_name, const char *session_id)
{
   if (!repo_path || !repo_path[0] || !branch_name || !branch_name[0] || !session_id || !session_id[0])
      return -1;
   const char *fields[] = {repo_path, branch_name, session_id};
   return write_result(call_stage(AIMEE_DB1_OP_OWNERSHIP_UPSERT, fields, 3, NULL, 0));
}

int db1_git_ownership_delete(const char *repo_path, const char *branch_name)
{
   if (!repo_path || !repo_path[0] || !branch_name || !branch_name[0])
      return -1;
   const char *fields[] = {repo_path, branch_name};
   return write_result(call_stage(AIMEE_DB1_OP_OWNERSHIP_DELETE, fields, 2, NULL, 0));
}

int db1_git_ownership_get_owner(const char *repo_path, const char *branch_name, char *owner_out, size_t owner_len)
{
   if (!repo_path || !repo_path[0] || !branch_name || !branch_name[0] || !owner_out || owner_len == 0)
      return -1;
   const char *fields[] = {repo_path, branch_name};
   int status = call_stage(AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, fields, 2, owner_out, owner_len);
   return read_result(status, owner_out);
}

int db1_git_ownership_get_branch_for_session(const char *repo_path, const char *session_id, char *branch_out, size_t branch_len)
{
   if (!repo_path || !repo_path[0] || !session_id || !session_id[0] || !branch_out || branch_len == 0)
      return -1;
   const char *fields[] = {repo_path, session_id};
   int status = call_stage(AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION, fields, 2, branch_out, branch_len);
   return read_result(status, branch_out);
}

int db1_git_ownership_find_session_by_prefix(const char *session_prefix, char *session_out, size_t session_len)
{
   if (!session_prefix || !session_prefix[0] || !session_out || session_len == 0)
      return -1;
   const char *fields[] = {session_prefix};
   int status = call_stage(AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX, fields, 1, session_out, session_len);
   return read_result(status, session_out);
}

int db1_session_feature_branch_upsert(const char *repo_path, const char *session_id, const char *feature_branch)
{
   if (!repo_path || !repo_path[0] || !session_id || !session_id[0] || !feature_branch || !feature_branch[0])
      return -1;
   const char *fields[] = {repo_path, session_id, feature_branch};
   return write_result(call_stage(AIMEE_DB1_OP_FEATURE_BRANCH_UPSERT, fields, 3, NULL, 0));
}

int db1_session_feature_branch_get(const char *repo_path, const char *session_id, char *branch_out, size_t branch_len)
{
   if (!repo_path || !repo_path[0] || !session_id || !session_id[0] || !branch_out || branch_len == 0)
      return -1;
   const char *fields[] = {repo_path, session_id};
   int status = call_stage(AIMEE_DB1_OP_FEATURE_BRANCH_GET, fields, 2, branch_out, branch_len);
   return read_result(status, branch_out);
}

/* clang-format on */
