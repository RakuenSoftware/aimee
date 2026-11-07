/* git_ownership.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "db1_client/git_ownership.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static db1_family_t family = {"db1.git_ownership", "git ownership", AIMEE_DB1_EVENT_GIT_OWNERSHIP, AIMEE_DB1_STAGE_GIT_OWNERSHIP, 2000, 0};


/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

/* A read answers found(1) / not-found(0) / error(-1), which is what the direct
   implementation returns and what its callers already branch on. */
static int read_result(int status, const char *value_out)
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
   return write_result(db1_call(&family, AIMEE_DB1_OP_OWNERSHIP_UPSERT, fields, 3, NULL, NULL, 0, NULL));
}

int db1_git_ownership_delete(const char *repo_path, const char *branch_name)
{
   if (!repo_path || !repo_path[0] || !branch_name || !branch_name[0])
      return -1;
   const char *fields[] = {repo_path, branch_name};
   return write_result(db1_call(&family, AIMEE_DB1_OP_OWNERSHIP_DELETE, fields, 2, NULL, NULL, 0, NULL));
}

int db1_git_ownership_get_owner(const char *repo_path, const char *branch_name, char *owner_out, size_t owner_len)
{
   if (!repo_path || !repo_path[0] || !branch_name || !branch_name[0] || !owner_out || owner_len == 0)
      return -1;
   const char *fields[] = {repo_path, branch_name};
   char *const values[] = {owner_out};
   const size_t caps[] = {owner_len};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_OWNERSHIP_OWNER_GET, fields, 2, values, caps, 1, NULL);
   return read_result(wire_status, owner_out);
}

int db1_git_ownership_get_branch_for_session(const char *repo_path, const char *session_id, char *branch_out, size_t branch_len)
{
   if (!repo_path || !repo_path[0] || !session_id || !session_id[0] || !branch_out || branch_len == 0)
      return -1;
   const char *fields[] = {repo_path, session_id};
   char *const values[] = {branch_out};
   const size_t caps[] = {branch_len};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION, fields, 2, values, caps, 1, NULL);
   return read_result(wire_status, branch_out);
}

int db1_git_ownership_find_session_by_prefix(const char *session_prefix, char *session_out, size_t session_len)
{
   if (!session_prefix || !session_prefix[0] || !session_out || session_len == 0)
      return -1;
   const char *fields[] = {session_prefix};
   char *const values[] = {session_out};
   const size_t caps[] = {session_len};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX, fields, 1, values, caps, 1, NULL);
   return read_result(wire_status, session_out);
}

int db1_session_feature_branch_upsert(const char *repo_path, const char *session_id, const char *feature_branch)
{
   if (!repo_path || !repo_path[0] || !session_id || !session_id[0] || !feature_branch || !feature_branch[0])
      return -1;
   const char *fields[] = {repo_path, session_id, feature_branch};
   return write_result(db1_call(&family, AIMEE_DB1_OP_FEATURE_BRANCH_UPSERT, fields, 3, NULL, NULL, 0, NULL));
}

int db1_session_feature_branch_get(const char *repo_path, const char *session_id, char *branch_out, size_t branch_len)
{
   if (!repo_path || !repo_path[0] || !session_id || !session_id[0] || !branch_out || branch_len == 0)
      return -1;
   const char *fields[] = {repo_path, session_id};
   char *const values[] = {branch_out};
   const size_t caps[] = {branch_len};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_FEATURE_BRANCH_GET, fields, 2, values, caps, 1, NULL);
   return read_result(wire_status, branch_out);
}

/* clang-format on */
