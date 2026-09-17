package memory

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

type commandStore struct {
	recordingDataStore
	domainDataStore
	err     error
	query   string
	limit   int
	deleted bool
}

func (s *commandStore) Get(ctx context.Context, scope Scope, id int64) (Record, error) {
	if s.err != nil {
		return Record{}, s.err
	}
	return s.recordingDataStore.Get(ctx, scope, id)
}
func (s *commandStore) Search(ctx context.Context, scope Scope, query, kind, tier string, limit int) ([]Record, error) {
	s.query, s.limit = query, limit
	if s.err != nil {
		return nil, s.err
	}
	return s.recordingDataStore.Search(ctx, scope, query, kind, tier, limit)
}
func (s *commandStore) Delete(_ context.Context, scope Scope, _ int64) (bool, error) {
	s.scope = scope
	return s.deleted, s.err
}
func (s *commandStore) Supersede(_ context.Context, scope Scope, id int64, content string, confidence float64) (Record, error) {
	s.scope = scope
	return Record{ID: id + 1, Scope: scope, Content: content, Confidence: confidence}, s.err
}
func (*commandStore) Feedback(context.Context, Scope, []int64, bool) error      { return nil }
func (*commandStore) Maintenance(context.Context, Scope) (int, int, int, error) { return 0, 0, 0, nil }
func (s *commandStore) Stats(context.Context) (MemoryStats, error)              { return MemoryStats{}, s.err }

func runPublicCommand(t *testing.T, client *Client, verb, args string) map[string]any {
	t.Helper()
	body, err := client.Command(context.Background(), 73, verb, json.RawMessage(args))
	if err != nil {
		t.Fatal(err)
	}
	var result map[string]any
	if err := json.Unmarshal(body, &result); err != nil {
		t.Fatal(err)
	}
	return result
}

func TestPrivateCommandsPreserveEnvelopesAndScope(t *testing.T) {
	s := &commandStore{deleted: true}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, s)))
	for _, test := range []struct{ verb, args, want string }{
		{"store", `{"key":"editor","content":"vim","project":"secret-project","scope":{"type":"global"},"authority":"user","operation":"visible-search"}`, `{"status":"ok","store":"user","id":41}`},
		{"list", `{}`, `{"status":"ok","store":"user","memories":[],"active_context_missing":false}`},
		{"search", `{"keywords":["editor","preference"]}`, `{"status":"ok","facts":[],"windows":[],"active_context_missing":false}`},
		{"delete", `{"id":41}`, `{"status":"ok","store":"user","id":41,"deleted":true,"destroyed":false}`},
	} {
		got := runPublicCommand(t, client, test.verb, test.args)
		var want map[string]any
		json.Unmarshal([]byte(test.want), &want)
		if !reflect.DeepEqual(got, want) {
			t.Errorf("%s: got=%v want=%v", test.verb, got, want)
		}
		if s.scope != (Scope{Type: ScopeUser, Value: "_user"}) {
			t.Fatalf("scope escaped: %+v", s.scope)
		}
	}
	if s.put.Tier != "L2" || s.put.Kind != "fact" || s.put.Confidence != 1 {
		t.Fatalf("defaults lost: %+v", s.put)
	}
	if s.query != "editor preference" || s.limit != 10 {
		t.Fatalf("search=%q limit=%d", s.query, s.limit)
	}
	got := runPublicCommand(t, client, "supersede", `{"old_id":41,"new_content":"new","confidence":0.5,"session_id":"s"}`)
	if got["id"] != float64(42) || got["content"] != "new" || got["status"] != "ok" || got["store"] != "user" {
		t.Fatalf("supersede=%v", got)
	}
	got = runPublicCommand(t, client, "get", `{"id":41,"workspace":"shared"}`)
	if got["memory"].(map[string]any)["id"] != float64(41) || got["store"] != "user" {
		t.Fatal(got)
	}
	got = runPublicCommand(t, client, "stats", `{}`)
	if got["stats"] == nil || got["store"] != "user" {
		t.Fatal(got)
	}
}

func TestPublicCommandValidation(t *testing.T) {
	s := &commandStore{}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, s)))
	for _, test := range []struct{ verb, args string }{
		{"get", `{"id":0}`}, {"get", `{"id":1.5}`}, {"get", `{"id":9007199254740992}`},
		{"get", `{"id":"42"}`}, {"get", `{"id":42,"as_of":null}`}, {"get", `{"id":42,"store":"kb"}`},
		{"get", `{"id":42,"store":null}`}, {"delete", `{"id":-1}`},
		{"store", `{"key":"","content":"x"}`}, {"store", `{"key":null,"content":"x"}`},
		{"store", `{"key":"x","content":{}}`}, {"store", `{"key":"x","content":"x","confidence":null}`},
		{"store", `{"key":"x","content":"x","confidence":false}`}, {"store", `{"key":"x","content":"x","confidence":-1}`},
		{"store", `{"key":"x","content":"x","confidence":1.01}`},
		{"supersede", `{"old_id":42,"new_content":""}`}, {"supersede", `{"old_id":42,"new_content":"x","session_id":null}`},
		{"search", `{"keywords":[]}`}, {"search", `{"keywords":[null]}`}, {"search", `{"keywords":[""]}`},
		{"search", `{"keywords":["x"],"limit":1.5}`}, {"search", `{"keywords":["x"],"limit":33}`},
	} {
		got := runPublicCommand(t, client, test.verb, test.args)
		if got["status"] != "error" || got["kind"] != "invalid_argument" {
			t.Errorf("%s %s: %v", test.verb, test.args, got)
		}
		if s.scope.Type != "" {
			t.Fatal("invalid command accessed storage")
		}
	}
	for _, confidence := range []string{"0", "0.25", "1"} {
		got := runPublicCommand(t, client, "store", `{"key":"x","content":"x","confidence":`+confidence+`}`)
		if got["status"] != "ok" {
			t.Fatal(got)
		}
	}
}

func TestPublicCommandMissingAndOutage(t *testing.T) {
	s := &commandStore{err: ErrMemoryNotFound}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, s)))
	for _, verb := range []string{"get", "supersede"} {
		got := runPublicCommand(t, client, verb, `{"id":42,"old_id":42,"new_content":"x"}`)
		if got["kind"] != "not_found" {
			t.Fatal(got)
		}
	}
	s.err = errors.New("fixture storage outage")
	got := runPublicCommand(t, client, "get", `{"id":42}`)
	if got["kind"] != "unavailable" {
		t.Fatal(got)
	}
	s.err = nil
	got = runPublicCommand(t, client, "delete", `{"id":42}`)
	if got["kind"] != "not_found" {
		t.Fatal(got)
	}
	// Bounded query compatibility: all keyword validation precedes truncation.
	args, _ := json.Marshal(map[string]any{"keywords": []string{strings.Repeat("a", 3000), "b"}})
	runPublicCommand(t, client, "search", string(args))
	if len(s.query) != 2047 {
		t.Fatalf("query len=%d", len(s.query))
	}
}

func TestCommandStageRejectsWrongPlacementAndMalformedFrames(t *testing.T) {
	frame, _ := bus.EncodeCommand("delete", json.RawMessage(`{"id":42}`))
	for _, placement := range []Placement{PlacementKB, ""} {
		_, status := NewHandler(nil, WithDataStore(placement, nil))(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(status)
		}
	}
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	for _, raw := range []string{`null`, `[]`, `42`} {
		frame, _ := bus.EncodeCommand("get", json.RawMessage(raw))
		_, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusInvalidRequest {
			t.Fatal(raw, status)
		}
	}
	_, status := handler(bus.ModuleInvocation{StageID: 999}, frame)
	if status != bus.ModuleStatusInvalidRequest {
		t.Fatal(status)
	}
}
