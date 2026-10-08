package memory

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/memory/hillock"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
)

type independentTransport struct{ client *http.Client }

func (e independentTransport) Do(ctx context.Context, _ uint64, r egress.HTTPRequest) (egress.HTTPResponse, error) {
	req, err := http.NewRequestWithContext(ctx, r.Request.Method, r.Request.TargetURL, stringsReader(r.Body))
	if err != nil {
		return egress.HTTPResponse{}, err
	}
	for k, v := range r.Headers {
		req.Header.Set(k, v)
	}
	resp, err := e.client.Do(req)
	if err != nil {
		return egress.HTTPResponse{}, err
	}
	defer resp.Body.Close()
	var raw json.RawMessage
	if err = json.NewDecoder(resp.Body).Decode(&raw); err != nil {
		return egress.HTTPResponse{}, err
	}
	return egress.HTTPResponse{Status: resp.StatusCode, Body: raw}, nil
}
func stringsReader(raw []byte) *bytes.Reader { return bytes.NewReader(raw) }

func TestExternalBackendHandlerWithoutNativeStorage(t *testing.T) {
	home := t.TempDir()
	if _, err := identity.Ensure(home, "server"); err != nil {
		t.Fatal(err)
	}
	t.Setenv("AIMEE_HOME", home)
	t.Setenv("AIMEE_MEMORY_BACKEND", "hillock")
	t.Setenv("AIMEE_MEMORY_BACKEND_AUTH", "none")
	// PostgreSQL is deliberately absent: the production external-store constructor
	// receives only egress and placement, and cannot be supplied a native source.
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/health" {
			json.NewEncoder(w).Encode(map[string]any{"version": 1, "stateless": true, "upstream_revision": hillock.UpstreamRevision})
			return
		}
		var input struct {
			Candidates []struct {
				ID       int64  `json:"id"`
				Revision string `json:"revision"`
			} `json:"candidates"`
		}
		if err := json.NewDecoder(r.Body).Decode(&input); err != nil {
			t.Error(err)
			return
		}
		hits := []map[string]any{}
		for _, c := range input.Candidates {
			hits = append(hits, map[string]any{"id": c.ID, "revision": c.Revision, "score": 1})
		}
		json.NewEncoder(w).Encode(map[string]any{"hits": hits})
	}))
	defer server.Close()
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", server.URL)
	executor := independentTransport{server.Client()}
	data, err := configuredExternalStore(executor, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	actor := &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "Virant", TransportIdentity: "discord"}
	makeHandler := func(data *externalDataStore) bus.ModuleHandler {
		return NewHandler(nil, WithDataStore(PlacementServer, data), func(options *handlerOptions) { options.commandContext = actor })
	}
	handler := makeHandler(data)
	call := func(request DataRequest) DataResponse {
		t.Helper()
		raw, _ := json.Marshal(request)
		reply, status := handler(bus.ModuleInvocation{StageID: StageData}, raw)
		if status != bus.ModuleStatusOK {
			t.Fatalf("%s: status %d", request.Operation, status)
		}
		var out DataResponse
		if err := json.Unmarshal(reply, &out); err != nil {
			t.Fatal(err)
		}
		return out
	}
	confidence := 1.0
	first := call(DataRequest{Operation: "store", Kind: "world_fact", Key: "Kibukx height", Content: "69cm", Confidence: &confidence, Authority: AuthorityUser, IdempotencyKey: "independent-create-0001"})
	if len(first.Records) != 1 || first.Records[0].ID <= 0 {
		t.Fatal(first)
	}
	record := first.Records[0]
	repeated := call(DataRequest{Operation: "store", Kind: "world_fact", Key: "Kibukx height", Content: "69cm", Confidence: &confidence, Authority: AuthorityUser, IdempotencyKey: "independent-create-0001"})
	if repeated.Records[0].ID != record.ID {
		t.Fatal("retry duplicated record")
	}
	data, err = configuredExternalStore(executor, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	handler = makeHandler(data)
	got := call(DataRequest{Operation: "get", ID: record.ID, IncludeVersion: true})
	if got.Records[0].Content != "69cm" || got.Records[0].Authorship.Principal != "Virant" {
		t.Fatal(got)
	}
	correction := call(DataRequest{Operation: "supersede", ID: record.ID, Content: "170cm", Confidence: &confidence, ExpectedVersion: record.Version, Authority: AuthorityUser})
	if correction.Records[0].Content != "170cm" {
		t.Fatal(correction)
	}
	conflict := call(DataRequest{Operation: "supersede", ID: record.ID, Content: "4 feet", Confidence: &confidence, ExpectedVersion: record.Version, Authority: AuthorityUser})
	if conflict.Code == nil || *conflict.Code != MutationVersionConflict {
		t.Fatal("stale update", conflict)
	}
	old := call(DataRequest{Operation: "get", ID: record.ID, AtVersion: record.Version})
	if old.Records[0].Content != "69cm" || !old.Records[0].Historical {
		t.Fatal(old)
	}
	actor.UserAuthority = false
	refused := call(DataRequest{Operation: "supersede", ID: record.ID, Content: "4 feet", Confidence: &confidence, Authority: AuthorityUser})
	if refused.Code == nil || *refused.Code != MutationReviewRequired {
		t.Fatal("body authority bypass", refused)
	}
	actor.UserAuthority = true
	snapshot := call(DataRequest{Operation: "backend-export"}).Payload
	if len(snapshot) == 0 {
		t.Fatal("no portable export")
	}
	// Switch engines: the same catalog format preserves source identity/history.
	t.Setenv("AIMEE_MEMORY_BACKEND", "cognee")
	alternative, err := configuredExternalStore(executor, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	if err = alternative.catalog.Import(context.Background(), snapshot); err != nil {
		t.Fatal(err)
	}
	migrated, err := alternative.Get(context.Background(), Scope{Type: ScopeUser, Value: "_user"}, record.ID)
	if err != nil || migrated.Content != "170cm" {
		t.Fatal(migrated, err)
	}
	// Exercise the public command boundary used by chatbot and RPC callers.
	t.Setenv("AIMEE_MEMORY_BACKEND", "hillock")
	kb, err := configuredExternalStore(executor, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	kbHandler := NewHandler(nil, WithDataStore(PlacementKB, kb))
	kbActor := *actor
	kbActor.ScopeKind, kbActor.ScopeID = "project", "discord"
	public := func(verb string, args map[string]any) map[string]any {
		t.Helper()
		args["project"], args["scope_context"] = "discord", true
		raw, _ := json.Marshal(args)
		out, status := invokeContextCommand(t, kbHandler, 0, kbActor, verb, string(raw))
		if status != bus.ModuleStatusOK || out["error"] != nil || out["kind"] != nil {
			t.Fatalf("public %s: %d %v", verb, status, out)
		}
		return out
	}
	created := public("store", map[string]any{"key": "Samy height", "content": "150cm", "authority": "user"})
	id, ok := created["id"].(float64)
	if !ok || id < 1 {
		t.Fatal(created)
	}
	public("get", map[string]any{"id": id, "include_version": true})
	public("update", map[string]any{"id": id, "content": "151cm", "authority": "user"})
	public("search", map[string]any{"view": "server", "keywords": []string{"Samy"}})
	public("find_facts_scoped", map[string]any{"query": "Samy", "scope_type": "project", "scope_value": "discord"})
	updated := public("get", map[string]any{"id": id})
	if !bytes.Contains(mustMarshal(updated), []byte("151cm")) {
		t.Fatal(updated)
	}
	public("backend_capabilities", map[string]any{})
	public("backend_list", map[string]any{})
	exported := public("backend_export", map[string]any{})
	destinationRoot := filepath.Join(t.TempDir(), "destination")
	t.Setenv("AIMEE_MEMORY_BACKEND_DIR", destinationRoot)
	destination, err := configuredExternalStore(executor, PlacementKB)
	if err != nil {
		t.Fatal(err)
	}
	destinationHandler := NewHandler(nil, WithDataStore(PlacementKB, destination))
	importArgs, _ := json.Marshal(map[string]any{"project": "discord", "scope_context": true, "snapshot": exported})
	imported, importStatus := invokeContextCommand(t, destinationHandler, 0, kbActor, "backend_import", string(importArgs))
	if importStatus != bus.ModuleStatusOK || imported["imported"] != true {
		t.Fatal(importStatus, imported)
	}
	copy, err := destination.Get(context.Background(), Scope{Type: ScopeProject, Value: "discord"}, int64(id))
	if err != nil || copy.Content != "151cm" {
		t.Fatal(copy, err)
	}
	modelCaller := kbActor
	modelCaller.UserAuthority = false
	_, exportStatus := invokeContextCommand(t, destinationHandler, 0, modelCaller, "backend_export", `{"project":"discord","scope_context":true}`)
	if exportStatus != bus.ModuleStatusCapabilityAbsent {
		t.Fatal("model export bypass", exportStatus)
	}
	t.Setenv("AIMEE_MEMORY_BACKEND_DIR", "")
	public("list", map[string]any{})
	public("delete", map[string]any{"id": id, "authority": "user"})
	t.Setenv("AIMEE_MEMORY_BACKEND", "hillock")
	dir := filepath.Join(home, "memory-backends", "hillock", string(PlacementServer))
	backup, err := os.ReadFile(filepath.Join(dir, "records.json"))
	if err != nil {
		t.Fatal(err)
	}
	raw := []byte(`{"operation":"erase-backend","backend_subject":"Virant"}`)
	if _, status := handler(bus.ModuleInvocation{StageID: StageData, PrincipalRef: 99}, raw); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatal("foreign erasure caller", status)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, raw); status != bus.ModuleStatusOK {
		t.Fatal("owner erasure", status)
	}
	if err = os.WriteFile(filepath.Join(dir, "records.json"), backup, 0600); err != nil {
		t.Fatal(err)
	}
	data, err = configuredExternalStore(executor, PlacementServer)
	if err != nil {
		t.Fatal(err)
	}
	handler = makeHandler(data)
	gone := call(DataRequest{Operation: "get", ID: record.ID})
	if len(gone.Records) != 0 {
		t.Fatal("restored erased record", gone)
	}
}

func mustMarshal(value any) []byte { raw, _ := json.Marshal(value); return raw }
