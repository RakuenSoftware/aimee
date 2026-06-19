/* workflow_api_stub.c -- trivial wf_api_* implementations for tests that link
 * server_http.o (which references the W7 /v1/workflow route handlers) but do not
 * exercise the workflow surface. The real logic is in server_workflow_api.c and
 * is covered by unit-test-wfe-webapi; here we only need the symbols to resolve
 * without pulling the wfe_ definition model + DB1 store into the link. */
#include "server/server_workflow_api.h"

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
int wf_api_item(const char *id, char *resp, int cap)
{
   (void)id;
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
void wfe_scheduler_notify(void)
{
}
