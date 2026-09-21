package memory

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestRecallGatePolicyLivesInGo(t *testing.T) {
	for _, test := range []struct {
		query string
		skip  bool
	}{
		{"thanks that worked", true},
		{"okay", true},
		{"what worked", false},
		{"fix server-go/modules/memory", false},
		{"release 0.4.1", false},
		{"", false},
	} {
		skip, _ := recallGateDecision(test.query)
		if skip != test.skip {
			t.Errorf("recallGateDecision(%q) = %v, want %v", test.query, skip, test.skip)
		}
	}
}

func TestRecallGateDoesNotRequireADataStore(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_RECALL_GATE", "enforce")
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	response, status := handler(bus.ModuleInvocation{StageID: StageData},
		dataRequest(t, DataRequest{Operation: "recall-gate", Query: "thanks"}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if !decoded.Skip || !decoded.Enforced || decoded.Reason != "acknowledgement" {
		t.Fatalf("response = %#v", decoded)
	}
}

func TestRedirectPolicyDoesNotRequireADataStore(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	response, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
		Operation: "redirect-classify", Client: "claude", Tool: "Write", Home: "/home/u",
		Path: "/home/u/.claude/projects/p/memory/note.md",
	}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.Verdict != redirectStore || decoded.Name != "note" {
		t.Fatalf("response = %#v", decoded)
	}

	response, status = handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
		Operation: "redirect-bash", Client: "claude", Home: "/home/u",
		Command: "printf x > /home/u/.claude/projects/p/memory/note.md",
	}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("bash status = %v", status)
	}
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.Verdict != redirectReject {
		t.Fatalf("bash response = %#v", decoded)
	}
}

func TestContentGateDoesNotRequireADataStore(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	response, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
		Operation: "content-gate", Content: "token=abc", ContentCapacity: 128,
	}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.SensitiveStatus != 1 || decoded.Redacted != "[REDACTED]" ||
		decoded.Classification != "restricted" {
		t.Fatalf("response = %#v", decoded)
	}
}

type recordingDataStore struct {
	scope  Scope
	scopes []Scope
	put    Record
}

func (s *recordingDataStore) ProspectiveCounts(context.Context) (int, int, int, int, error) {
	return 1, 2, 3, 4, nil
}

func (s *recordingDataStore) RecallFacts(_ context.Context, entity, query string,
	turnRequestsSensitive bool, capacity int) (string, int, error) {
	if entity == "Ada" && query == "" && turnRequestsSensitive && capacity == 128 {
		return "- role: engineer\n", 1, nil
	}
	return "", 0, nil
}

func (s *recordingDataStore) ValidAt(_ context.Context, id int64, asOf string) (bool, error) {
	return id == 41 && asOf == "2026-06-12 00:00:00", nil
}

func (s *recordingDataStore) Get(_ context.Context, scope Scope, id int64) (Record, error) {
	s.scope = scope
	return Record{ID: id, Scope: scope}, nil
}

func (s *recordingDataStore) Search(_ context.Context, scope Scope, _, _, _ string, _ int) ([]Record, error) {
	s.scope = scope
	s.scopes = append(s.scopes, scope)
	return []Record{}, nil
}

func (s *recordingDataStore) Put(_ context.Context, scope Scope, record Record) (Record, error) {
	s.scope, s.put = scope, record
	record.ID = 41
	return record, nil
}

func (s *recordingDataStore) Delete(_ context.Context, scope Scope, _ int64) (bool, error) {
	s.scope = scope
	return true, nil
}

func dataRequest(t *testing.T, request DataRequest) []byte {
	t.Helper()
	body, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	return body
}

func float64ptr(value float64) *float64 { return &value }

func TestServerPlacementOwnsOnlyUserMemory(t *testing.T) {
	backend := &recordingDataStore{}
	handler := NewHandler(nil, WithDataStore(PlacementServer, backend))
	request := DataRequest{Operation: "store", Kind: "preference", Key: "editor",
		Content: "vim", Confidence: float64ptr(.9)}
	response, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.scope != (Scope{Type: ScopeUser, Value: "_user"}) {
		t.Fatalf("scope = %#v", backend.scope)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if len(decoded.Records) != 1 || decoded.Records[0].ID != 41 {
		t.Fatalf("response = %#v", decoded)
	}

	request.Scope = Scope{Type: ScopeProject, Value: "secret-project"}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("server accepted project memory: %v", status)
	}
}

func TestSearchPatternAllowsTextBetweenKeywordClusters(t *testing.T) {
	if got, want := searchPattern("HTTP restart"), "%HTTP%restart%"; got != want {
		t.Fatalf("searchPattern() = %q, want %q", got, want)
	}
	if got := searchPattern("  \t "); got != "%" {
		t.Fatalf("empty searchPattern() = %q, want %%", got)
	}
}

func TestKBPlacementOwnsOnlyKBScopes(t *testing.T) {
	backend := &recordingDataStore{}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	request := DataRequest{Operation: "search", Scope: Scope{Type: ScopeWorkspace, Value: "/srv/dev"}, Limit: 10}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusOK {
		t.Fatalf("workspace status = %v", status)
	}
	if backend.scope != request.Scope {
		t.Fatalf("scope = %#v", backend.scope)
	}

	request.Scope = Scope{Type: ScopeUser, Value: "alice"}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("kb accepted user memory: %v", status)
	}
}

func TestKBVisibleSearchExpandsScopeInGo(t *testing.T) {
	backend := &recordingDataStore{}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	request := DataRequest{Operation: "visible-search", Query: "cache", Workspace: "/srv/dev",
		Project: "aimee", Limit: 10}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	want := []Scope{{Type: ScopeProject, Value: "aimee"}, {Type: ScopeWorkspace, Value: "/srv/dev"},
		{Type: ScopeGlobal, Value: "_global"}}
	if len(backend.scopes) != len(want) {
		t.Fatalf("scopes = %#v", backend.scopes)
	}
	for i := range want {
		if backend.scopes[i] != want[i] {
			t.Fatalf("scopes[%d] = %#v, want %#v", i, backend.scopes[i], want[i])
		}
	}
}

func TestWorkflowIdentityIsConstructedInGo(t *testing.T) {
	backend := &recordingDataStore{}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	request := DataRequest{Operation: "upsert-workflow", Workspace: "/srv/dev", SignalType: "tests",
		Rule: "run unit tests", Confidence: float64ptr(.8), SessionID: "session-1"}
	if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if backend.scope != (Scope{Type: ScopeWorkspace, Value: "/srv/dev"}) ||
		backend.put.Key != "workflow:/srv/dev:tests" || backend.put.Kind != "workflow" || backend.put.Tier != "L1" {
		t.Fatalf("scope=%#v record=%#v", backend.scope, backend.put)
	}
}

func TestProspectiveCountsAreKBOnly(t *testing.T) {
	backend := &recordingDataStore{}
	kbHandler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	response, status := kbHandler(bus.ModuleInvocation{StageID: StageData},
		dataRequest(t, DataRequest{Operation: "prospective-count"}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("kb status = %v", status)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.Armed != 1 || decoded.Triggered != 2 || decoded.Completed != 3 ||
		decoded.ProspectiveExpired != 4 {
		t.Fatalf("response = %#v", decoded)
	}
	serverHandler := NewHandler(nil, WithDataStore(PlacementServer, backend))
	if _, status = serverHandler(bus.ModuleInvocation{StageID: StageData},
		dataRequest(t, DataRequest{Operation: "prospective-count"})); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("server accepted KB prospective count: %v", status)
	}
}

func TestFactRecallIsImplementedByKBMemoryModule(t *testing.T) {
	backend := &recordingDataStore{}
	kbHandler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	response, status := kbHandler(bus.ModuleInvocation{StageID: StageData},
		dataRequest(t, DataRequest{Operation: "fact-recall", Entity: "Ada",
			TurnRequestsSensitive: true, ContentCapacity: 128}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("kb status = %v", status)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.Block == nil || *decoded.Block != "- role: engineer\n" ||
		decoded.Count == nil || *decoded.Count != 1 {
		t.Fatalf("response = %#v", decoded)
	}

	serverHandler := NewHandler(nil, WithDataStore(PlacementServer, backend))
	if _, status = serverHandler(bus.ModuleInvocation{StageID: StageData},
		dataRequest(t, DataRequest{Operation: "fact-recall", Entity: "Ada",
			ContentCapacity: 128})); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("server accepted KB fact recall: %v", status)
	}
}

func TestTemporalMemoryCheckIsImplementedByKBMemoryModule(t *testing.T) {
	backend := &recordingDataStore{}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	response, status := handler(bus.ModuleInvocation{StageID: StageData},
		dataRequest(t, DataRequest{Operation: "valid-at", ID: 41,
			AsOf: "2026-06-12 00:00:00"}))
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var decoded DataResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.ValidAt == nil || !*decoded.ValidAt {
		t.Fatalf("response = %#v", decoded)
	}
}

func TestPlacementParsingIsExplicit(t *testing.T) {
	for _, value := range []string{"", "local", "SERVER-KB"} {
		if _, err := ParsePlacement(value); err == nil {
			t.Fatalf("ParsePlacement(%q) succeeded", value)
		}
	}
	if placement, err := ParsePlacement(" KB "); err != nil || placement != PlacementKB {
		t.Fatalf("ParsePlacement = %q, %v", placement, err)
	}
}
