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
#include "log.h"
#include "module_json_call.h"

#include <stdio.h>
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

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static int encode(uint8_t *out, size_t out_sz, uint32_t op, const char *const *fields,
                  uint32_t count, uint32_t *len_out)
{
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      if (!fields[i] || !fields[i][0])
         return -1;
      size_t n = strlen(fields[i]);
      /* Refuse here rather than let the module refuse: an over-long field is a
         caller bug, and the round trip would only rename it. */
      if (n >= AIMEE_DB1_FIELD_MAX)
         return -1;
      need += 4u + n;
   }
   if (need > out_sz)
      return -1;

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
   *len_out = at;
   return 0;
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

   uint8_t request[8u + AIMEE_DB1_FIELDS_MAX * (4u + AIMEE_DB1_FIELD_MAX)];
   uint32_t request_len = 0;
   if (encode(request, sizeof request, op, fields, count, &request_len) != 0)
      return -1;

   uint8_t response[8u + AIMEE_DB1_FIELD_MAX];
   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_GIT_OWNERSHIP_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_GIT_OWNERSHIP, AIMEE_DB1_STAGE_GIT_OWNERSHIP, 0, deadline,
                           request, request_len, response, (uint32_t)sizeof response,
                           &response_len, NULL, NULL);
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
   {
      warn_unreachable((int)rc);
      return -1;
   }

   uint32_t status = aimee_db1_get_u32(response);
   uint32_t payload_len = aimee_db1_get_u32(response + 4u);
   /* A reply whose declared length disagrees with what arrived is not a reply
      to read part of. */
   if (payload_len != response_len - 8u)
      return -1;
   if (value_out && value_len)
   {
      if (payload_len >= value_len)
         return -1;
      memcpy(value_out, response + 8u, payload_len);
      value_out[payload_len] = '\0';
   }
   return (int)status;
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
   if (!repo_path || !branch_name || !session_id)
      return -1;
   const char *fields[] = {repo_path, branch_name, session_id};
   return write_result(call_stage(AIMEE_DB1_OP_OWNERSHIP_UPSERT, fields, 3, NULL, 0));
}

int db1_git_ownership_delete(const char *repo_path, const char *branch_name)
{
   if (!repo_path || !branch_name)
      return -1;
   const char *fields[] = {repo_path, branch_name};
   return write_result(call_stage(AIMEE_DB1_OP_OWNERSHIP_DELETE, fields, 2, NULL, 0));
}

int db1_git_ownership_get_owner(const char *repo_path, const char *branch_name, char *owner_out, size_t owner_len)
{
   if (!repo_path || !branch_name || !owner_out || owner_len == 0)
      return -1;
   const char *fields[] = {repo_path, branch_name};
   int status = call_stage(AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, fields, 2, owner_out, owner_len);
   return read_result(status, owner_out);
}

int db1_git_ownership_get_branch_for_session(const char *repo_path, const char *session_id, char *branch_out, size_t branch_len)
{
   if (!repo_path || !session_id || !branch_out || branch_len == 0)
      return -1;
   const char *fields[] = {repo_path, session_id};
   int status = call_stage(AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION, fields, 2, branch_out, branch_len);
   return read_result(status, branch_out);
}

int db1_git_ownership_find_session_by_prefix(const char *session_prefix, char *session_out, size_t session_len)
{
   if (!session_prefix || !session_out || session_len == 0)
      return -1;
   const char *fields[] = {session_prefix};
   int status = call_stage(AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX, fields, 1, session_out, session_len);
   return read_result(status, session_out);
}

int db1_session_feature_branch_upsert(const char *repo_path, const char *session_id, const char *feature_branch)
{
   if (!repo_path || !session_id || !feature_branch)
      return -1;
   const char *fields[] = {repo_path, session_id, feature_branch};
   return write_result(call_stage(AIMEE_DB1_OP_FEATURE_BRANCH_UPSERT, fields, 3, NULL, 0));
}

int db1_session_feature_branch_get(const char *repo_path, const char *session_id, char *branch_out, size_t branch_len)
{
   if (!repo_path || !session_id || !branch_out || branch_len == 0)
      return -1;
   const char *fields[] = {repo_path, session_id};
   int status = call_stage(AIMEE_DB1_OP_FEATURE_BRANCH_GET, fields, 2, branch_out, branch_len);
   return read_result(status, branch_out);
}

/* clang-format on */
