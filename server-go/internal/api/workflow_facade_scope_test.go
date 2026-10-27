package api

import (
	"bytes"
	"encoding/json"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/internal/workflowstore"
	"github.com/JBailes/aimee/server-go/modules/workflows"
)

// Only issuance is a unit seam. This emits the actual C facade's private wire;
// Go control/API and DB1 PostgreSQL persistence below are real owners. It is not
// a fixture HMAC or a substitute for live authenticated source issuance.
const workflowFacadeBridgeC = `
#include "server_http_identity.h"
#include "server_native_delegate_source.h"
#include "server/workflow_control_bus.h"
#include "headers/module_json_call.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int server_native_delegate_source_mint_for_workspace(const char *p,const char *w,char out[65],char *e,size_t n) {
 (void)e;(void)n;assert(!strcmp(p,"workflow")&&w&&w[0]);memset(out,'a',64);out[64]=0;return 0;
}
int server_native_delegate_source_revoke(const char *scope){(void)scope;return 0;}
int obs_bus_module_available(uint32_t kind){return kind==AIMEE_WORKFLOWS_EVENT_CONTROL;}
const char *aimee_module_call_result_name(aimee_module_call_result_t value){(void)value;return "fixture";}
cJSON *aimee_module_json_call_raw(uint32_t kind,uint32_t stage,const char *body,size_t len,size_t max,int timeout,aimee_module_call_result_t *result) {
 (void)max;(void)timeout;assert(kind==AIMEE_WORKFLOWS_EVENT_CONTROL&&stage==AIMEE_WORKFLOWS_STAGE_CONTROL);
 assert(fwrite(body,1,len,stdout)==len);*result=AIMEE_MODULE_CALL_OK;return cJSON_Parse("{\"status\":200,\"body\":\"{}\"}");
}
int main(int argc,char **argv) {
 request_context_t owner={0};owner.vault_transport=ATTEST_WEBCHAT_TRUSTED;strcpy(owner.vault_principal,"webuser:alice");
 server_http_identity_bind_request_owner(&owner);
 if(argc>1&&!strcmp(argv[1],"projection")) {
  assert(!strcmp(server_http_identity_principal(),"webuser:alice"));
  assert(!strcmp(server_http_identity_workflow_principal(),"alice"));
  owner.vault_transport=ATTEST_UDS_PEERCRED;server_http_identity_bind_request_owner(&owner);
  assert(!strcmp(server_http_identity_workflow_principal(),"webuser:alice"));
  strcpy(owner.vault_principal,"server");server_http_identity_bind_request_owner(&owner);
  assert(!strcmp(server_http_identity_workflow_principal(),"server"));return 0;
 }
 char body[8192],response[256];size_t n=fread(body,1,sizeof(body)-1,stdin);assert(n&&n<sizeof(body)-1);body[n]=0;
 return workflow_control_request("POST","/v1/dev/submit","",body,(int)n,server_http_identity_workflow_principal(),argc>1&&!strcmp(argv[1],"operator"),response,sizeof response)==200?0:1;
}
`

func workflowFacadeBridge(t *testing.T) string {
	t.Helper()
	source, err := filepath.Abs("../../../src")
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	code := filepath.Join(dir, "bridge.c")
	if err := os.WriteFile(code, []byte(workflowFacadeBridgeC), 0600); err != nil {
		t.Fatal(err)
	}
	binary := filepath.Join(dir, "bridge")
	args := []string{"-std=c11", "-D_GNU_SOURCE", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-I" + source, "-I" + filepath.Join(source, "headers"), "-I" + filepath.Join(source, "vendor/headers"), "-I" + filepath.Join(source, "modules/vault"), "-I" + filepath.Join(source, "modules/kb/c"), "-I" + filepath.Join(source, "db1_client"), "-I" + filepath.Join(source, "peer_client"), "-I" + filepath.Join(source, "pipeline")}
	for _, pattern := range []string{"modules/*/include", "modules/*", "core/*/include"} {
		dirs, err := filepath.Glob(filepath.Join(source, pattern))
		if err != nil {
			t.Fatal(err)
		}
		for _, dir := range dirs {
			if info, err := os.Stat(dir); err == nil && info.IsDir() {
				args = append(args, "-I"+dir)
			}
		}
	}
	if os.Getenv("AIMEE_TEST_C_SANITIZERS") == "1" {
		args = append(args, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g")
	}
	args = append(args, code, filepath.Join(source, "server/workflow_control_bus.c"), filepath.Join(source, "server/server_http_identity.c"), filepath.Join(source, "vendor/cJSON.c"), "-lm", "-o", binary)
	if output, err := exec.Command("cc", args...).CombinedOutput(); err != nil {
		t.Fatalf("actual C workflow facade bridge: %v\n%s", err, output)
	}
	return binary
}
func TestWorkflowCFacadePreservesAttestedAccountProjection(t *testing.T) {
	if output, err := exec.Command(workflowFacadeBridge(t), "projection").CombinedOutput(); err != nil {
		t.Fatalf("canonical actor/account projection: %v %s", err, output)
	}
}
func TestWorkflowCFacadeScopePersistsThroughRealGoDB1(t *testing.T) {
	server, store, _ := newTestServer(t)
	definition := t.TempDir()
	if err := os.WriteFile(filepath.Join(definition, "build.yaml"), []byte("name: build\nstart: source\nnodes:\n  - id: source\n    block: author.proposal\n"), 0600); err != nil {
		t.Fatal(err)
	}
	server.workflowDir = definition
	// A pre-cutover run keeps its bare account spelling and must remain visible.
	if err := store.CreateWorkItem(t.Context(), workflowstore.CreateWorkItem{ID: "legacy-owned", Repo: "/approved", ProposalPath: "legacy", WorkflowName: "build", StartStage: "source", Submitter: "alice"}); err != nil {
		t.Fatal(err)
	}
	bridge := workflowFacadeBridge(t)
	body, _ := json.Marshal(map[string]any{"proposal_md": "owned proposal", "repo": t.TempDir(), "workflow": "build"})
	command := exec.Command(bridge)
	command.Stdin = bytes.NewReader(body)
	wire, err := command.Output()
	if err != nil {
		t.Fatal(err)
	}
	var envelope workflows.ControlRequest
	if err := json.Unmarshal(wire, &envelope); err != nil {
		t.Fatal(err)
	}
	scope := strings.Repeat("a", 64)
	if envelope.Principal != "alice" || envelope.ExecutionScope != scope || envelope.WorkflowOperator {
		t.Fatal("C private owner context lost")
	}
	handler := workflows.NewControlHandler(server)
	reply, status := handler(bus.ModuleInvocation{StageID: workflows.StageControl}, wire)
	if status != bus.ModuleStatusOK {
		t.Fatal("control owner failed", status)
	}
	var response workflows.ControlResponse
	if err := json.Unmarshal(reply, &response); err != nil {
		t.Fatal(err)
	}
	if response.Status != http.StatusOK {
		t.Fatalf("owner rejected actual C envelope: %d %s", response.Status, response.Body)
	}
	var created struct {
		ID string `json:"work_item_id"`
	}
	if err := json.Unmarshal([]byte(response.Body), &created); err != nil || created.ID == "" {
		t.Fatal(err)
	}
	item, err := store.WorkItem(t.Context(), created.ID)
	if err != nil || item.ExecutionScope != scope || item.Submitter != "alice" {
		t.Fatal("actual DB1 lost immutable source/account", err)
	}
	public, err := json.Marshal(item)
	if err != nil || bytes.Contains(public, []byte(scope)) || bytes.Contains(public, []byte("execution_scope")) {
		t.Fatal("public item exposed source authority", err)
	}
	items, err := store.WorkItems(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 {
		t.Fatalf("old/new owned records lost: %d", len(items))
	}
	if err := store.CreateWorkItem(t.Context(), workflowstore.CreateWorkItem{ID: "other-owned", Repo: "/approved", ProposalPath: "other", WorkflowName: "build", StartStage: "source", Submitter: "bob"}); err != nil {
		t.Fatal(err)
	}
	read := envelope
	read.Method = "GET"
	read.Path = "/v1/workflow/items"
	read.Body = ""
	readWire, _ := json.Marshal(read)
	readReply, readStatus := handler(bus.ModuleInvocation{StageID: workflows.StageControl}, readWire)
	var readResponse workflows.ControlResponse
	if readStatus != bus.ModuleStatusOK || json.Unmarshal(readReply, &readResponse) != nil || readResponse.Status != 200 {
		t.Fatal("old/new account lookup failed")
	}
	if !strings.Contains(readResponse.Body, "legacy-owned") || !strings.Contains(readResponse.Body, created.ID) || strings.Contains(readResponse.Body, "other-owned") {
		t.Fatal("canonical projection lost old ownership or disclosed another account")
	}
	operator := exec.Command(bridge, "operator")
	operator.Stdin = bytes.NewReader(body)
	operatorWire, err := operator.Output()
	if err != nil {
		t.Fatal(err)
	}
	var operatorEnvelope workflows.ControlRequest
	if json.Unmarshal(operatorWire, &operatorEnvelope) != nil || !operatorEnvelope.WorkflowOperator || operatorEnvelope.Principal != "alice" {
		t.Fatal("attested operator capability lost")
	}
	// Public model/user body cannot override the trusted C outer envelope.
	forged, _ := json.Marshal(map[string]any{"proposal_md": "forged", "repo": t.TempDir(), "workflow": "build", "execution_scope": strings.Repeat("b", 64)})
	command = exec.Command(bridge)
	command.Stdin = bytes.NewReader(forged)
	wire, err = command.Output()
	if err != nil {
		t.Fatal(err)
	}
	reply, status = handler(bus.ModuleInvocation{StageID: workflows.StageControl}, wire)
	if status != bus.ModuleStatusOK || json.Unmarshal(reply, &response) != nil || response.Status != 400 {
		t.Fatal("public scope override reached storage")
	}
	items, err = store.WorkItems(t.Context())
	if err != nil || len(items) != 3 {
		t.Fatal("forged body created work", err)
	}
}
