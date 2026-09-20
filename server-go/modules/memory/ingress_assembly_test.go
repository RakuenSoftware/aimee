package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestIngressBlockNativeGoldens(t *testing.T) {
	for _, test := range []struct {
		entries []ingressEntry
		budget  int
		want    string
		omitted int
	}{
		{[]ingressEntry{{"code", "C:\n", "a\n"}, {"memory", "M:\n", "b\n"}}, 1000,
			"C:\na\n\nM:\nb\ncontext-budget: used_bytes=11 budget_bytes=1000 omitted_count=0 headline_missing_count=0\n", 0},
		{[]ingressEntry{{"code", "C:\n", "AAAAAAAAAA\n"}, {"code", "C:\n", "x\n"}}, 394, "C:\nx\n", 1},
		{[]ingressEntry{{"code", "C:\n", "AAAAAAAAAA\n"}, {"memory", "M:\n", "y\n"}}, 394, "M:\ny\n", 1},
		{nil, 1000, "", 0},
	} {
		got, omitted := ingressRenderBlock(test.entries, test.budget, 0)
		if got != test.want || omitted != test.omitted {
			t.Fatalf("got %q (%d), want %q (%d)", got, omitted, test.want, test.omitted)
		}
	}
}

func TestIngressEnvelopeNoInstructionsAndConfidenceBoundaries(t *testing.T) {
	for _, test := range []struct {
		score float64
		band  string
	}{{0, "low"}, {.329999, "low"}, {.33, "medium"}, {.659999, "medium"}, {.66, "high"}, {1, "high"}} {
		block := "recommended:\n  - src/a.c::f"
		want := "<aimee-context confidence=\"" + test.band + "\">\n" + block + "\n</aimee-context>"
		if got := ingressEnvelope(block, test.score); got != want {
			t.Fatal(got)
		}
		if got := ingressEnvelope(block+"\n", test.score); got != want {
			t.Fatal("extra newline", got)
		}
	}
	for _, block := range []string{"", " \t\n\r "} {
		if got := ingressEnvelope(block, 1); got != "" {
			t.Fatal(got)
		}
	}
}

func TestIngressAssemblyBothPlacementsAndExactIDs(t *testing.T) {
	request := `{"operation":"ingress-assemble","budget":2000,"code":[{"file_path":"file.go","snippet":"<x>\n\t&","line":42}],
"memories":[{"id":"9223372036854775807","key":"a<b","headline":"summary","score":0.88},
{"id":"102","content":"fallback","score":0.44}],"facts_requested":true,"facts_response":{"status":"ok","facts":"fact"},
"temporal":"temporal\n","audit":"audit"}`
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		r := runHostRuntime(t, handler, request)
		block := r["block"].(string)
		for _, want := range []string{"recommended (code):\n  - file.go\n    > &lt;x&gt; &amp;\n",
			"memory:9223372036854775807 a&lt;b [?/memory score=0.880 headline_missing=false]",
			"memory:102 [?/memory score=0.440 headline_missing=true]", "## Known facts\nfact\n",
			"recommended (temporal learning):\ntemporal\n", "recommended (audit context):\naudit\n"} {
			if !strings.Contains(block, want) {
				t.Fatal(want, block)
			}
		}
		if !strings.HasPrefix(r["envelope"].(string), `<aimee-context confidence="medium">`) ||
			r["headline_missing_count"] != float64(1) || r["facts_unavailable"] != false {
			t.Fatal(r)
		}
		frame, _ := bus.EncodeCommand("runtime", []byte(request))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
	}
}

func TestIngressAssemblyFoldingAndBounds(t *testing.T) {
	request := ingressAssemblyRequest{Budget: 2000, Compress: true, Code: []ingressCodeHit{
		{FilePath: "fold.go", Snippet: strings.Repeat("é", 50), Line: 42},
		{FilePath: "no-line.go", Snippet: strings.Repeat("é", 90)},
		{FilePath: "short.go", Snippet: "short", Line: 1},
	}}
	r, err := ingressAssemble(request)
	if err != nil {
		t.Fatal(err)
	}
	block := r["block"].(string)
	if !strings.Contains(block, "code — expand via code_span_get") || !strings.Contains(block, "fold.go:42\n") ||
		!strings.Contains(block, "no-line.go\n    > ") || !strings.Contains(block, "short.go\n    > short") ||
		r["folded_count"] != 1 || r["folded_saved"] != 100 || !utf8.ValidString(block) {
		t.Fatal(r)
	}
	request.Compress = false
	r, _ = ingressAssemble(request)
	if strings.Contains(r["block"].(string), "fold.go:42") || r["folded_count"] != 0 {
		t.Fatal(r)
	}
	request.Budget = 384
	r, _ = ingressAssemble(request)
	if r["block"] != "" || r["envelope"] != "" || r["omitted_count"] != 3 {
		t.Fatal(r)
	}
	request.Code = make([]ingressCodeHit, 7)
	if _, err := ingressAssemble(request); err == nil {
		t.Fatal("unbounded code accepted")
	}
	request.Code = nil
	request.Memories = make([]ingressMemoryPreview, 6)
	if _, err := ingressAssemble(request); err == nil {
		t.Fatal("unbounded memories accepted")
	}
}

func TestIngressAssemblyFactsFailureAndTaskConfidence(t *testing.T) {
	for _, facts := range []string{`null`, `{"status":"error","facts":"must not inject"}`, `{"status":"ok","facts":1}`, `{}`} {
		r, err := ingressAssemble(ingressAssemblyRequest{TaskBlock: "task\n", TaskConfidence: .95, FactsRequested: true, FactsResponse: json.RawMessage(facts)})
		if err != nil || r["facts_unavailable"] != true || strings.Contains(r["block"].(string), "Known facts") ||
			!strings.HasPrefix(r["envelope"].(string), `<aimee-context confidence="high">`) {
			t.Fatal(facts, r, err)
		}
	}
	r, err := ingressAssemble(ingressAssemblyRequest{FactsRequested: true, FactsResponse: json.RawMessage(`{"status":"ok","facts":""}`)})
	if err != nil || r["facts_unavailable"] != false || r["envelope"] != "" {
		t.Fatal(r, err)
	}
	if _, err := ingressAssemble(ingressAssemblyRequest{Memories: []ingressMemoryPreview{{ID: "9223372036854775808"}}}); err == nil {
		t.Fatal("overflow identity accepted")
	}
}

func TestVersionedIngressByteBudget(t *testing.T) {
	for _, limit := range []int{0, 1, 384, 512, 1024} {
		request := ingressAssemblyRequest{Budget: 9000,
			ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &limit},
			Memories:      []ingressMemoryPreview{{ID: "9007199254740993", Key: "small", Content: "do not erase 界"}},
			Code:          []ingressCodeHit{{FilePath: strings.Repeat("large", 500)}},
		}
		r, err := ingressAssemble(request)
		if err != nil {
			t.Fatal(err)
		}
		envelope := r["envelope"].(string)
		a := r["context_accounting"].(ContextAccounting)
		if len(envelope) > limit || a.RenderedBytes != len(envelope) || a.MaxContextBytes != limit || a.CountState != "exact" || a.Unit != "utf8_bytes" || a.Boundary != "memory_envelope" || a.TokenCountState != "unavailable" {
			t.Fatal("serialized byte accounting mismatch", limit, r)
		}
		retained := r["retained_memory_ids"].([]string)
		if limit <= 384 && (envelope != "" || len(retained) != 0) {
			t.Fatal("literal zero/small byte limit inherited a larger legacy budget", r)
		}
		if limit == 1024 && (len(retained) != 1 || retained[0] != "9007199254740993" || !strings.Contains(envelope, "do not erase 界")) {
			t.Fatal("oversized first item displaced small evidence", r)
		}
	}
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		for _, tt := range []struct{ limits, kind string }{
			{`{"schema_version":2}`, "unsupported_version"},
			{`{"schema_version":1,"max_context_tokens":0}`, "unsupported_mode"},
			{`{"schema_version":1,"max_request_tokens":2000}`, "unsupported_mode"},
			{`{"schema_version":1,"reserved_response_tokens":100}`, "unsupported_mode"},
			{`{"schema_version":1,"max_context_bytes":-1}`, "invalid_argument"},
		} {
			r := runHostRuntime(t, handler, `{"operation":"ingress-assemble","context_limits":`+tt.limits+`}`)
			if r["status"] != "error" || r["kind"] != tt.kind || r["envelope"] != nil {
				t.Fatal("unsupported or invalid budget silently accepted", tt, r)
			}
		}
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"ingress-assemble","context_limits":{"schema_version":1,"max_context_byte":0}}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("unknown limit silently ignored", status)
		}
	}
}
