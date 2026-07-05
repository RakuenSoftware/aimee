/* workflow_api_stub.c -- trivial wf_api_* implementations for tests that link
 * server_http.o (which references the W7 /v1/workflow route handlers) but do not
 * exercise the workflow surface. The real logic is in server_workflow_api.c and
 * is covered by unit-test-wfe-webapi; here we only need the symbols to resolve
 * without pulling the wfe_ definition model + DB1 store into the link. */
#include "server/server_workflow_api.h"
#include "wfe_approval.h" /* wfe_approval_present/record */
#include "wfe_def.h"      /* wfe_def_t, wfe_gate_reject_t, wfe_def_free */
#include "wfe_engine.h"   /* wfe_load_workflow */

#include <stddef.h>
#include <stdio.h>

static int stub(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{}");
   return 200;
}

int wf_api_blocks(char *resp, int cap)
{
   return stub(resp, cap);
}
int wf_api_block_put(const char *name, const char *body, char *resp, int cap)
{
   (void)name;
   (void)body;
   return stub(resp, cap);
}
int wf_api_block_delete(const char *name, char *resp, int cap)
{
   (void)name;
   return stub(resp, cap);
}
int wf_api_list(char *resp, int cap)
{
   return stub(resp, cap);
}
int wf_api_get(const char *name, char *resp, int cap)
{
   (void)name;
   return stub(resp, cap);
}
int wf_api_validate(const char *body, char *resp, int cap)
{
   (void)body;
   return stub(resp, cap);
}
int wf_api_save(const char *body, char *resp, int cap)
{
   (void)body;
   return stub(resp, cap);
}
int wf_api_items(char *resp, int cap)
{
   return stub(resp, cap);
}
int wf_api_items_all(char *resp, int cap)
{
   return stub(resp, cap);
}
int wf_api_item(const char *id, char *resp, int cap)
{
   (void)id;
   return stub(resp, cap);
}
int wf_api_events(const char *id, long after, int limit, char *resp, int cap)
{
   (void)id;
   (void)after;
   (void)limit;
   return stub(resp, cap);
}
int wf_api_proposal(const char *id, char *resp, int cap)
{
   (void)id;
   return stub(resp, cap);
}
int wf_api_item_pause(const char *id, int is_operator, char *resp, int cap)
{
   (void)id;
   (void)is_operator;
   return stub(resp, cap);
}
int wf_api_item_resume(const char *id, int is_operator, char *resp, int cap)
{
   (void)id;
   (void)is_operator;
   return stub(resp, cap);
}
int wf_api_item_stop(const char *id, int is_operator, char *resp, int cap)
{
   (void)id;
   (void)is_operator;
   return stub(resp, cap);
}
int wf_api_item_delete(const char *id, int is_operator, char *resp, int cap)
{
   (void)id;
   (void)is_operator;
   return stub(resp, cap);
}
int wf_api_repo_tree(const char *rel, char *resp, int cap)
{
   (void)rel;
   return stub(resp, cap);
}
int wf_api_repo_file(const char *rel, char *resp, int cap)
{
   (void)rel;
   return stub(resp, cap);
}

/* Autonomous-development intake symbols referenced by rh_dev_submit in
 * server_http_routes.inc. Stubbed so tests that link server_http.o don't pull the
 * wfe engine + scheduler + DB1 store; the real path is covered by
 * unit-test-wfe-scheduler / unit-test-wfe-engine. */
int wfe_work_item_create(const char *workflow_name, const char *repo, const char *proposal_path,
                         const char *mode, char out_id[80], char *err, size_t errlen)
{
   (void)workflow_name;
   (void)repo;
   (void)proposal_path;
   (void)mode;
   (void)err;
   (void)errlen;
   if (out_id)
      snprintf(out_id, 80, "stub-wi");
   return 0;
}
int wfe_work_item_resolve(const char *workflow_name, const char *repo, char out_name[64],
                          char out_ver[65], char out_start[64], char out_repo[512], char out_id[80],
                          char *err, size_t errlen)
{
   (void)workflow_name;
   (void)repo;
   (void)err;
   (void)errlen;
   if (out_name)
      snprintf(out_name, 64, "build");
   if (out_ver)
      snprintf(out_ver, 65, "v1");
   if (out_start)
      snprintf(out_start, 64, "intake");
   if (out_repo)
      snprintf(out_repo, 512, "%s", repo ? repo : "");
   if (out_id)
      snprintf(out_id, 80, "stub-wi");
   return 0;
}
void wfe_scheduler_notify(void)
{
}

/* PC2: the /v1/dev/ci-event route entry keeps rh_workflow_gate live under LTO, so
 * these wfe symbols must resolve in this minimal-link test (it does not exercise the
 * workflow-gate route). Trivial stubs — the real logic is covered by unit-test-wfe-*. */
int wfe_approval_present(const char *work_item_id, const char *gate, const char *content_hash)
{
   (void)work_item_id;
   (void)gate;
   (void)content_hash;
   return 0;
}
int wfe_approval_record(const char *work_item_id, const char *gate, const char *content_hash,
                        const char *actor)
{
   (void)work_item_id;
   (void)gate;
   (void)content_hash;
   (void)actor;
   return 0;
}
wfe_def_t *wfe_load_workflow(const char *name, char *err, size_t errlen)
{
   (void)name;
   if (err && errlen)
      err[0] = '\0';
   return NULL;
}
void wfe_def_free(wfe_def_t *def)
{
   (void)def;
}
wfe_gate_reject_t wfe_gate_reject_target(const wfe_def_t *def, const char *gate_id,
                                         char *out_target, size_t out_n)
{
   (void)def;
   (void)gate_id;
   if (out_target && out_n)
      out_target[0] = '\0';
   return WFE_GATE_REJECT_TERMINAL;
}
