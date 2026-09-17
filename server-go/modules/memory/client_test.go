package memory

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

type clientCallFunc func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)

func (f clientCallFunc) Call(ctx context.Context, event, stage uint32, trace uint64, timeout time.Duration, body []byte) ([]byte, error) {
	return f(ctx, event, stage, trace, timeout, body)
}

func clientForHandler(t *testing.T, handler bus.ModuleHandler) *Client {
	t.Helper()
	client, err := NewClient(clientCallFunc(func(ctx context.Context, event, stage uint32, trace uint64, timeout time.Duration, body []byte) ([]byte, error) {
		if event != 5888+stage || stage < 1 || stage > 7 {
			t.Errorf("wrong event/stage: %d/%d", event, stage)
		}
		if trace != 73 || timeout <= 0 || timeout > time.Second {
			t.Errorf("lost call metadata: trace=%d timeout=%s", trace, timeout)
		}
		response, status := handler(bus.ModuleInvocation{StageID: stage, TraceID: trace, PrincipalClass: 1, PrincipalRef: 200}, body)
		if status != bus.ModuleStatusOK {
			return nil, &bus.ModuleCallStatusError{Status: status}
		}
		return response, nil
	}), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	return client
}

func TestClientGateAndExtractionConformance(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil))
	ctx := context.Background()
	for _, test := range gateMatrixCases(t) {
		if len(test.relType) > relTypeMax {
			continue
		}
		got, err := client.CheckFact(ctx, 73, test.head, test.relType, test.tail)
		if err != nil || got != test.want {
			t.Fatalf("gate(%q) = %d, %v; want %d", test.relType, got, err, test.want)
		}
	}
	for _, row := range fixtureRows(t, "testdata/extract_corpus.tsv") {
		text := unescapeField(row[0])
		got, err := client.Extract(ctx, 73, text, 16)
		want := ExtractPatterns(text, 16)
		if err != nil || len(got) != len(want) {
			t.Fatalf("extract(%q): %v, %v; want %v", text, got, err, want)
		}
		for i := range got {
			if got[i] != want[i] {
				t.Fatalf("extract(%q): got %v, want %v", text, got, want)
			}
		}
		scan, err := client.ScanTurn(ctx, 73, text)
		attr, has := PossessiveAttr(text)
		if err != nil || scan != (TurnScan{IsRetraction(text), has, attr}) {
			t.Fatalf("scan(%q): %+v, %v", text, scan, err)
		}
	}
	for _, test := range []struct {
		score int64
		band  uint32
	}{{-1, 1}, {329999, 1}, {330000, 2}, {659999, 2}, {660000, 3}} {
		got, err := client.Rerank(ctx, 73, test.score)
		if err != nil || got != test.band {
			t.Fatalf("rerank(%d): %d, %v", test.score, got, err)
		}
	}
	commands, err := client.Commands(ctx, 73)
	if err != nil || !reflect.DeepEqual(commands, DeclaredCommands()) {
		t.Fatalf("commands: %+v, %v", commands, err)
	}
}

func TestClientPrivacyConformance(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil))
	for _, row := range fixtureRows(t, "testdata/pii_turns.tsv") {
		text := unescapeField(row[0])
		got, err := client.RequestsSensitive(context.Background(), 73, text)
		if err != nil || got != TurnRequestsSensitive(text) {
			t.Fatalf("PII turn %q: %v, %v", text, got, err)
		}
	}
	var relations []string
	for _, row := range fixtureRows(t, "testdata/pii_sensitivity.tsv") {
		relations = append(relations, unescapeField(row[0]))
	}
	got, err := client.Sensitivities(context.Background(), 73, relations)
	if err != nil || len(got) != len(relations) {
		t.Fatalf("sensitivities: %v", err)
	}
	for i, relation := range relations {
		if got[i] != RelSensitivityOf(relation) {
			t.Fatalf("sensitivity %q = %d", relation, got[i])
		}
	}
}

func TestClientDataPreservesPlacementAndLargeIDs(t *testing.T) {
	for _, test := range []struct {
		placement       Placement
		allowed, denied Scope
	}{
		{PlacementServer, Scope{}, Scope{Type: ScopeProject, Value: "private-project"}},
		{PlacementKB, Scope{Type: ScopeWorkspace, Value: "/srv/project"}, Scope{Type: ScopeUser, Value: "alice"}},
	} {
		t.Run(string(test.placement), func(t *testing.T) {
			store := &recordingDataStore{}
			client := clientForHandler(t, NewHandler(nil, WithDataStore(test.placement, store)))
			request := DataRequest{Operation: "store", Scope: test.allowed, Kind: "preference", Key: "editor", Content: "vim", Confidence: float64ptr(.9)}
			got, err := client.Data(context.Background(), 73, request)
			if err != nil || len(got.Records) != 1 || got.Records[0].ID != 41 {
				t.Fatalf("store: %+v, %v", got, err)
			}
			request.Scope = test.denied
			_, err = client.Data(context.Background(), 73, request)
			var status *bus.ModuleCallStatusError
			if !errors.As(err, &status) || status.Status != bus.ModuleStatusInvalidRequest {
				t.Fatalf("cross-placement call: %v", err)
			}
		})
	}
	const id int64 = 9007199254740993
	client, _ := NewClient(clientCallFunc(func(_ context.Context, _, _ uint32, _ uint64, _ time.Duration, body []byte) ([]byte, error) {
		if !bytes.Contains(body, []byte(`"id":9007199254740993`)) {
			t.Fatalf("request ID lost precision: %s", body)
		}
		return []byte(`{"records":[{"id":9007199254740993}]}`), nil
	}), 0)
	response, err := client.Data(context.Background(), 0, DataRequest{Operation: "get", ID: id})
	if err != nil || len(response.Records) != 1 || response.Records[0].ID != id {
		t.Fatalf("ID roundtrip: %+v, %v", response, err)
	}
}

func TestClientEmbeddingsPreserveOwnerOutcomes(t *testing.T) {
	for _, body := range []string{
		`{"dim":2,"vector":[0.25,0.5]}`,
		`{"dim":0,"unavailable":true,"retry_after_ms":30}`,
		`{"dim":0,"unauthorized":true,"error":"not authorized"}`,
		`{"dim":1,"vector":[0.25],"truncated":true}`,
		`{"dim":0,"serving_id":"model-v1"}`,
	} {
		client, _ := NewClient(clientCallFunc(func(_ context.Context, event, stage uint32, _ uint64, _ time.Duration, _ []byte) ([]byte, error) {
			if event != EventEmbed || stage != StageEmbed {
				t.Fatalf("wrong embedding route")
			}
			return []byte(body), nil
		}), 0)
		got, err := client.Embed(context.Background(), 0, EmbedRequest{Text: "test", MaxDim: 2})
		var want EmbedResponse
		if err := clientJSONObject([]byte(body), &want); err != nil {
			t.Fatal(err)
		}
		if err != nil || !reflect.DeepEqual(got, want) {
			t.Fatalf("embed: %+v, %v; want %+v", got, err, want)
		}
	}
}

func clientOperations(c *Client) map[string]func(context.Context) error {
	return map[string]func(context.Context) error{
		"rerank": func(ctx context.Context) error { _, e := c.Rerank(ctx, 73, 660000); return e },
		"write": func(ctx context.Context) error {
			_, e := c.CheckFact(ctx, 73, NodePerson, "works_for", NodeOrg)
			return e
		},
		"extract":     func(ctx context.Context) error { _, e := c.Extract(ctx, 73, "Alice works for Acme", 16); return e },
		"scan":        func(ctx context.Context) error { _, e := c.ScanTurn(ctx, 73, "my editor is vim"); return e },
		"pii":         func(ctx context.Context) error { _, e := c.RequestsSensitive(ctx, 73, "my email"); return e },
		"sensitivity": func(ctx context.Context) error { _, e := c.Sensitivities(ctx, 73, []string{"email"}); return e },
		"commands":    func(ctx context.Context) error { _, e := c.Commands(ctx, 73); return e },
		"data": func(ctx context.Context) error {
			_, e := c.Data(ctx, 73, DataRequest{Operation: "recall-gate", Query: "thanks"})
			return e
		},
		"embed": func(ctx context.Context) error {
			_, e := c.Embed(ctx, 73, EmbedRequest{Text: "test", MaxDim: 2})
			return e
		},
	}
}

func TestClientNoFallbackOrRetriesOnTransportFailure(t *testing.T) {
	for _, failure := range []error{bus.ErrModuleCallCapabilityAbsent, bus.ErrModuleCallRejected, bus.ErrModuleCallDeadline, bus.ErrModuleCallCancelled, &bus.ModuleCallStatusError{Status: bus.ModuleStatusInternal}} {
		calls := 0
		client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
			calls++
			return nil, failure
		}), 0)
		for name, invoke := range clientOperations(client) {
			before := calls
			if err := invoke(context.Background()); !errors.Is(err, failure) || calls != before+1 {
				t.Fatalf("%s failure became success/retry: %v, calls=%d", name, err, calls-before)
			}
		}
	}
}

func TestClientDeadlineAndCancellation(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	client, _ := NewClient(clientCallFunc(func(got context.Context, _, _ uint32, _ uint64, deadline time.Duration, _ []byte) ([]byte, error) {
		if got != ctx || deadline <= 0 || deadline > time.Second {
			t.Fatalf("deadline/context lost: %v, %s", got, deadline)
		}
		return []byte(`{"records":[]}`), nil
	}), time.Minute)
	if _, err := client.Data(ctx, 73, DataRequest{Operation: "get", ID: 1}); err != nil {
		t.Fatal(err)
	}
	cancel()
	for name, invoke := range clientOperations(client) {
		if err := invoke(ctx); !errors.Is(err, context.Canceled) {
			t.Fatalf("%s ignored cancellation: %v", name, err)
		}
	}
}

func TestClientRejectsMalformedReplies(t *testing.T) {
	// None of these replies can be interpreted as an empty successful result.
	for _, reply := range [][]byte{nil, {}, {1, 2, 3}, []byte("null"), []byte("[]"), []byte("{}"), []byte("{} {}"), []byte(`{"records":"bad"}`)} {
		client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
			return reply, nil
		}), 0)
		for name, invoke := range clientOperations(client) {
			if err := invoke(context.Background()); !errors.Is(err, ErrClientResponse) {
				t.Fatalf("%s accepted %q: %v", name, reply, err)
			}
		}
	}
}

func TestClientRejectsCorruptBinaryReplies(t *testing.T) {
	handler := NewHandler(nil)
	for _, mutation := range []string{"truncated", "trailing", "magic", "count-or-enum", "version-or-value"} {
		client, _ := NewClient(clientCallFunc(func(_ context.Context, _, stage uint32, _ uint64, _ time.Duration, body []byte) ([]byte, error) {
			reply, status := handler(bus.ModuleInvocation{StageID: stage}, body)
			if status != bus.ModuleStatusOK {
				t.Fatalf("fixture failed: %v", status)
			}
			switch mutation {
			case "truncated":
				reply = reply[:len(reply)-1]
			case "trailing":
				reply = append(reply, 0)
			case "magic":
				reply[0] ^= 0xff
			case "count-or-enum":
				binary.LittleEndian.PutUint32(reply[4:], ^uint32(0))
			case "version-or-value":
				binary.LittleEndian.PutUint32(reply[4:], 99)
			}
			return reply, nil
		}), 0)
		for name, invoke := range clientOperations(client) {
			if name == "data" || name == "embed" {
				continue
			}
			if err := invoke(context.Background()); !errors.Is(err, ErrClientResponse) {
				t.Fatalf("%s/%s accepted corrupt reply: %v", name, mutation, err)
			}
		}
	}
}

func TestClientRejectsOversizeBeforeDispatch(t *testing.T) {
	calls := 0
	client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
		calls++
		return nil, nil
	}), 0)
	ctx := context.Background()
	if _, err := client.CheckFact(ctx, 0, NodePerson, strings.Repeat("x", relTypeMax+1), NodeOrg); !errors.Is(err, ErrClientRequest) {
		t.Fatal(err)
	}
	if _, err := client.Sensitivities(ctx, 0, []string{strings.Repeat("x", relTypeMax+1)}); !errors.Is(err, ErrClientRequest) {
		t.Fatal(err)
	}
	if _, err := client.Extract(ctx, 0, "text", 0); !errors.Is(err, ErrClientRequest) {
		t.Fatal(err)
	}
	if _, err := client.DataJSON(ctx, 0, []byte(`{"operation":"get","unknown":true}`)); !errors.Is(err, ErrClientRequest) {
		t.Fatal(err)
	}
	if _, err := client.Data(ctx, 0, DataRequest{Operation: "store", Content: strings.Repeat("x", maxDataBody)}); !errors.Is(err, ErrClientRequest) {
		t.Fatal(err)
	}
	if calls != 0 {
		t.Fatalf("invalid requests dispatched %d times", calls)
	}
}

func TestClientConcurrentCallsKeepRequestIdentity(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil))
	var group sync.WaitGroup
	for i := range 40 {
		group.Add(1)
		go func() {
			defer group.Done()
			score := int64(i%3) * 330000
			got, err := client.Rerank(context.Background(), 73, score)
			if err != nil || got != uint32(i%3+1) {
				t.Errorf("score %d: %d, %v", score, got, err)
			}
		}()
	}
	group.Wait()
}

// Fixed v1 bytes keep caller/handler changes from drifting together unnoticed.
func TestClientPinnedWireRequests(t *testing.T) {
	tests := []struct {
		name         string
		event, stage uint32
		hex          string
		padding      int
	}{
		{"rerank", 5893, 5, "4d524e4b0100000020120a0000000000", 0},
		{"write", 5890, 2, "57475254010000000a0000000e00000009000000776f726b735f666f72", 247},
		{"extract", 5889, 1, "58545251010000001000000014000000416c69636520776f726b7320666f722041636d65", 0},
		{"scan", 5889, 1, "5254525101000000100000006d7920656469746f722069732076696d", 0},
		{"pii", 5892, 4, "5049525101000000080000006d7920656d61696c", 0},
		{"sensitivity", 5892, 4, "5053525101000000010000000500656d61696c", 0},
		{"commands", 5894, 6, "44434d4401000000", 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			want, err := hex.DecodeString(test.hex)
			if err != nil {
				t.Fatal(err)
			}
			want = append(want, make([]byte, test.padding)...)
			client, _ := NewClient(clientCallFunc(func(_ context.Context, event, stage uint32, _ uint64, _ time.Duration, request []byte) ([]byte, error) {
				if event != test.event || stage != test.stage || !bytes.Equal(request, want) {
					t.Fatalf("wire %d/%d %x; want %d/%d %x", event, stage, request, test.event, test.stage, want)
				}
				return nil, bus.ErrModuleCallCapabilityAbsent
			}), 0)
			if err := clientOperations(client)[test.name](context.Background()); !errors.Is(err, bus.ErrModuleCallCapabilityAbsent) {
				t.Fatal(err)
			}
		})
	}
}

func TestSensitivityRejectsImpossibleCountBeforeAllocation(t *testing.T) {
	request := make([]byte, 12)
	binary.LittleEndian.PutUint32(request, sensRequestMagic)
	binary.LittleEndian.PutUint32(request[4:], wireVersion)
	for _, count := range []uint32{1, 100, 1<<31 - 1, ^uint32(0)} {
		binary.LittleEndian.PutUint32(request[8:], count)
		if _, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve}, request); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("count %d accepted: %v", count, status)
		}
	}
}

func TestClientDataJSONPreservesResponseAndRequestBytes(t *testing.T) {
	request := []byte(`{ "operation":"get", "id":3, "scope":{"type":"project","value":"p"} }`)
	response := []byte(`{ "records":[], "future_diagnostic":{"flag":true} }`)
	client, _ := NewClient(clientCallFunc(func(_ context.Context, _, _ uint32, _ uint64, _ time.Duration, got []byte) ([]byte, error) {
		if !bytes.Equal(request, got) {
			t.Fatalf("request rewritten: %s", got)
		}
		return response, nil
	}), 0)
	got, err := client.DataJSON(context.Background(), 0, request)
	if err != nil || !bytes.Equal(got, response) {
		t.Fatalf("response rewritten: %s, %v", got, err)
	}
}

func FuzzClientResponseDecoders(f *testing.F) {
	for _, seed := range [][]byte{nil, []byte(`{"records":[]}`), []byte(`{"dim":0}`), {0x4d, 0x43, 0x4e, 0x46, 3, 0, 0, 0}} {
		f.Add(seed)
	}
	f.Fuzz(func(t *testing.T, response []byte) {
		client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
			return response, nil
		}), 0)
		for _, invoke := range clientOperations(client) {
			_ = invoke(context.Background())
		}
	})
}

func TestClientFactFailureCannotLookAccepted(t *testing.T) {
	for _, failure := range []error{nil, bus.ErrModuleCallCapabilityAbsent} {
		client, _ := NewClient(clientCallFunc(func(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error) {
			return nil, failure
		}), 0)
		verdict, err := client.CheckFact(context.Background(), 0, NodePerson, "works_for", NodeOrg)
		if err == nil || verdict == FactAccept {
			t.Fatalf("failed gate returned accept: %d, %v", verdict, err)
		}
	}
}

func TestClientEmbeddingThroughOwner(t *testing.T) {
	resetBreaker(t)
	upstream := &embedStub{status: 200, reply: `[0.25,0.5,0.75]`}
	endpoint := upstream.start(t)
	client := clientForHandler(t, NewHandler(allowEgress))
	response, err := client.Embed(context.Background(), 73, EmbedRequest{BaseURL: endpoint, Text: "a memory", InputType: "query", MaxDim: 3})
	if err != nil || response.Error != "" || response.Dim != 3 || !reflect.DeepEqual(response.Vector, []float32{.25, .5, .75}) {
		t.Fatalf("embedding: %+v, %v", response, err)
	}
	if upstream.calls != 1 || upstream.body != "a memory" {
		t.Fatalf("embedding request: %+v", upstream)
	}
}
