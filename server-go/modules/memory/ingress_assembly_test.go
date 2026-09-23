package memory

import (
	"encoding/json"
	"errors"
	"fmt"
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

func TestIngressLimitsCannotRaiseHostAllocation(t *testing.T) {
	for _, tc := range []struct {
		name, limits string
		want         int
	}{
		{"absent", "", 700},
		{"inherited", `,"context_limits":{"schema_version":1}`, 700},
		{"larger", `,"context_limits":{"schema_version":1,"max_context_bytes":4000}`, 700},
		{"equal", `,"context_limits":{"schema_version":1,"max_context_bytes":700}`, 700},
		{"smaller", `,"context_limits":{"schema_version":1,"max_context_bytes":500}`, 500},
		{"zero", `,"context_limits":{"schema_version":1,"max_context_bytes":0}`, 0},
	} {
		for _, placement := range []Placement{PlacementKB, PlacementServer} {
			t.Run(fmt.Sprintf("%s/%s", tc.name, placement), func(t *testing.T) {
				handler := NewHandler(nil, WithDataStore(placement, nil))
				// The long optional fact fits only if the request improperly raises
				// the operator's allocation. A small memory should still survive.
				facts, _ := json.Marshal(strings.Repeat("optional 界 ", 120))
				args := `{"operation":"ingress-assemble","budget":700,"memories":[{"id":"42","content":"retain this"}],"facts_requested":true,"facts_response":{"status":"ok","facts":` + string(facts) + `}` + tc.limits + `}`
				result := runHostRuntime(t, handler, args)
				accounting := result["context_accounting"].(map[string]any)
				envelope := result["envelope"].(string)
				if accounting["max_context_bytes"] != float64(tc.want) || len(envelope) > tc.want || strings.Contains(envelope, "optional") {
					t.Fatal("request escaped host allocation", result)
				}
				if tc.want == 700 && (!strings.Contains(envelope, "retain this") || len(result["retained_memory_ids"].([]any)) != 1) {
					t.Fatal("small retained evidence was lost", result)
				}
				plan := runHostRuntime(t, handler, `{"operation":"ingress-begin","query":"repair resolver","project":"p","session":"s","active_scope":true,"preview_enabled":true,"mode":"on","budget":700`+tc.limits+`}`)
				if tc.want == 0 {
					if plan["active"] != false {
						t.Fatal("zero allocated retrieval work", plan)
					}
				} else if plan["assembly"].(map[string]any)["budget"] != float64(tc.want) {
					t.Fatal("planner raised host allocation", plan)
				}
			})
		}
	}
}

func TestMemoryLimitsRejectAmbiguousWireValues(t *testing.T) {
	malformed := []string{
		`null`, `[]`, `{"schema_version":1,"MAX_CONTEXT_BYTES":0}`,
		`{"schema_version":1,"max_context_bytes":0,"Max_Context_Bytes":700}`,
		`{"schema_version":1,"max_context_bytes":0,"max_context_byt\u0065s":700}`,
		`{"schema_version":1,"max_context_bytes":1.5}`,
		`{"schema_version":1,"max_context_bytes":"700"}`,
		`{"schema_version":1,"max_context_bytes":9223372036854775808}`,
	}
	for _, field := range []string{"schema_version", "max_context_bytes", "max_context_tokens", "max_request_tokens", "reserved_response_tokens", "reserved_tool_tokens"} {
		prefix := `{"schema_version":1,`
		if field == "schema_version" {
			prefix = `{`
		}
		malformed = append(malformed, prefix+`"`+field+`":null}`, prefix+`"`+field+`":0,"`+field+`":1}`)
	}
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		operations := []string{"ingress-begin", "ingress-assemble"}
		if placement == PlacementKB {
			operations = append(operations, "typed-context")
		}
		for _, operation := range operations {
			for _, raw := range malformed {
				frame, err := bus.EncodeCommand("runtime", []byte(`{"operation":"`+operation+`","query":"fixture","project":"p","active_scope":true,"preview_enabled":true,"context_limits":`+raw+`}`))
				if err != nil {
					t.Fatal(err)
				}
				if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
					t.Fatalf("%s/%s accepted ambiguous limit %s: %v", placement, operation, raw, status)
				}
			}
		}
	}
	// Direct decoding must also reject bytes that encoding/json would repair.
	var limits ContextLimits
	if json.Unmarshal([]byte("{\"schema_version\":1,\"\xff\":0}"), &limits) == nil {
		t.Fatal("invalid UTF-8 accepted")
	}
}

func TestIngressRetainedEvidenceMatchesRenderedSelection(t *testing.T) {
	request := ingressAssemblyRequest{Budget: 1100, TaskBlock: "task\n",
		Code:     []ingressCodeHit{{FilePath: strings.Repeat("x", 2000)}, {FilePath: "retained.go", Snippet: "actual code"}},
		Memories: []ingressMemoryPreview{{ID: "9223372036854775807", Headline: strings.Repeat("界", 100) + "OMITTED_SENTINEL"}},
	}
	r, err := ingressAssemble(request)
	if err != nil {
		t.Fatal(err)
	}
	code := r["retained_code_indices"].([]int)
	memories := r["retained_memories"].([]ingressRetainedMemory)
	if len(code) != 1 || code[0] != 1 || len(memories) != 1 || memories[0].ID != "9223372036854775807" {
		t.Fatal("selected entry offsets confused with source indices", r)
	}
	preview := memories[0].Preview
	if preview != ingressSingleLine(request.Memories[0].Headline, 220) || strings.Contains(preview, "OMITTED_SENTINEL") || !strings.Contains(r["envelope"].(string), preview) {
		t.Fatal("evidence differs from rendered preview", r)
	}
	request.Budget = 384
	r, err = ingressAssemble(request)
	if err != nil || r["envelope"] != "" || len(r["retained_memories"].([]ingressRetainedMemory)) != 0 || len(r["retained_code_indices"].([]int)) != 0 {
		t.Fatal("empty envelope emitted evidence", r, err)
	}
}

func typedIngressFixture(t *testing.T) *typedContextResult {
	t.Helper()
	cfg := typedTestOptions(t, `{}`)
	cfg.Flags["working_context"] = true
	r := newTypedContext(DataRequest{TypedContext: cfg})
	r.add("observations", typedItem{id: "9223372036854775807", text: "limit 7", value: json.RawMessage(`{"revision":9223372036854775807,"text":"limit 7 界"}`)})
	r.add("working_context", typedItem{id: "turn:1", text: "large", value: map[string]any{"text": strings.Repeat("LARGE_ROW", 90)}})
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	if len(r.Retained) != 2 {
		t.Fatal(r)
	}
	return r
}

func TestIngressTypedProjectionRepackingAndIdentity(t *testing.T) {
	source := typedIngressFixture(t)
	raw, err := json.Marshal(source)
	if err != nil {
		t.Fatal(err)
	}
	for _, budget := range []int{0, 384, 700, 1100, 4096} {
		t.Run(fmt.Sprint(budget), func(t *testing.T) {
			request := ingressAssemblyRequest{TypedContextJSON: string(raw), ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &budget},
				Code: []ingressCodeHit{{FilePath: "retained.go", Snippet: "small code"}}}
			result, err := ingressAssemble(request)
			if err != nil {
				t.Fatal(err)
			}
			envelope := result["envelope"].(string)
			typed := result["typed_projection"].(map[string]any)
			refs := typed["retained_items"].([]typedProjectionRef)
			evidence := result["retained_typed_refs"].([]ingressProjectionEvidenceRef)
			if len(evidence) != len(refs) {
				t.Fatal("unselected items received evidence", result)
			}
			for i, ref := range refs {
				if evidence[i].Type != "memory_projection_item" || evidence[i].Ref != "typed:v1:"+typed["selection_digest"].(string)+":"+ref.Channel+":"+ref.ID {
					t.Fatal("evidence does not identify the accepted projection item", result)
				}
			}
			if len(envelope) > budget || typed["source_projection_digest"] != source.ProjectionDigest || typed["source_selection_digest"] != source.SelectionDigest || typed["omitted_count"] != 2-len(refs) {
				t.Fatal(result)
			}
			if budget <= 384 && (len(refs) != 0 || typed["rendered_bytes"] != 0 || envelope != "") {
				t.Fatal(result)
			}
			if budget == 700 || budget == 1100 {
				if len(refs) != 1 || refs[0].ID != "9223372036854775807" || !strings.Contains(envelope, `"revision":9223372036854775807`) || strings.Contains(envelope, "LARGE_ROW") || !strings.Contains(envelope, "small code") {
					t.Fatal("outer repacking lost small evidence or numeric identity", result)
				}
			}
			if budget == 4096 && (len(refs) != 2 || typed["selection_digest"] != source.SelectionDigest || !strings.Contains(envelope, source.Rendered)) {
				t.Fatal(result)
			}
			accounting := typed["context_accounting"].(ContextAccounting)
			if accounting.Digest != typed["projection_digest"] || accounting.RenderedBytes != typed["rendered_bytes"] || typed["selection_digest"] != typedSelectionDigest(accounting.Digest, refs) {
				t.Fatal("final identity does not bind final selection", result)
			}
			// Exercise JSON transport on both supported host placements too.
			args := map[string]any{"operation": "ingress-assemble", "typed_context_json": string(raw), "context_limits": request.ContextLimits, "code": request.Code}
			wire, _ := json.Marshal(args)
			for _, placement := range []Placement{PlacementKB, PlacementServer} {
				got := runHostRuntime(t, NewHandler(nil, WithDataStore(placement, nil)), string(wire))
				if got["envelope"] != envelope {
					t.Fatal("host runtime changed projection", got)
				}
			}
		})
	}
}

func TestIngressFactSourceProjection(t *testing.T) {
	const id = "9007199254743001"
	const block = "- role: engineer 界\n"
	makeSource := func() *factProjection {
		version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: "00000000-0000-0000-0000-000000000001", RecordID: id, RecordRevision: "7"}
		return newFactProjection(block, []typedProjectionRef{{Channel: "facts", ID: id, Source: &typedSourceVersion{Kind: "semantic_assertion", Version: version, MemoryParentState: "observed"}}})
	}
	assemble := func(text string, source *factProjection, budget int) (map[string]any, error) {
		raw, err := json.Marshal(map[string]any{"status": "ok", "facts": text, "fact_projection": source})
		if err != nil {
			t.Fatal(err)
		}
		return ingressAssemble(ingressAssemblyRequest{Budget: budget, FactsRequested: true, FactsResponse: raw,
			ContextLimits: &ContextLimits{SchemaVersion: 1, MaxContextBytes: &budget}})
	}
	source := makeSource()
	got, err := assemble(block, source, 4096)
	if err != nil {
		t.Fatal(err)
	}
	projection := got["facts_projection"].(map[string]any)
	refs := got["retained_fact_refs"].([]ingressProjectionEvidenceRef)
	if projection["selection_digest"] != source.SelectionDigest || len(refs) != 1 || refs[0].Ref != "facts:v1:"+source.SelectionDigest+":semantic_assertion:"+id || !strings.Contains(got["envelope"].(string), "## Known facts\n"+block) {
		t.Fatal(got)
	}
	got, err = assemble(block, source, 0)
	if err != nil {
		t.Fatal(err)
	}
	projection = got["facts_projection"].(map[string]any)
	if len(got["retained_fact_refs"].([]ingressProjectionEvidenceRef)) != 0 || len(projection["retained_items"].([]typedProjectionRef)) != 0 || projection["source_version_state"] != "unavailable" || projection["source_selection_digest"] != source.SelectionDigest || projection["omitted_count"] != 1 || got["envelope"] != "" {
		t.Fatal(got)
	}
	for _, change := range []string{"text", "revision", "parent", "channel", "id", "duplicate", "null", "count"} {
		p, text := makeSource(), block
		switch change {
		case "text":
			text = strings.Replace(block, "engineer", "operator", 1)
		case "revision":
			p.Retained[0].Source.Version.RecordRevision = "8"
		case "parent":
			p.Retained[0].Source.MemoryParentState = "unavailable"
		case "channel":
			p.Retained[0].Channel = "current_assertions"
		case "id":
			p.Retained[0].ID = "1"
		case "duplicate":
			p.Retained = append(p.Retained, p.Retained[0])
			p.SelectionDigest = typedSelectionDigest(p.ProjectionDigest, p.Retained)
		case "null":
			p = nil
		case "count":
			p.RenderedBytes++
		}
		result, err := assemble(text, p, 4096)
		var refusal *contextBudgetError
		if result != nil || !errors.As(err, &refusal) || refusal.kind != "invalid_projection" {
			t.Fatal("invalid fact binding accepted", change, result, err)
		}
	}
}

func TestIngressRejectsMismatchedTypedProjection(t *testing.T) {
	for _, mutation := range []string{"rendered", "selection", "digest", "version", "channels", "accounting", "rows", "references"} {
		t.Run(mutation, func(t *testing.T) {
			source := typedIngressFixture(t)
			switch mutation {
			case "rendered":
				source.Rendered = strings.Replace(source.Rendered, "limit 7", "limit 9", 1)
			case "selection":
				source.Retained[0].ID = "other"
			case "digest":
				source.ProjectionDigest = "sha256:wrong"
			case "version":
				source.ProjectionVersion = 2
			case "channels":
				source.Channels["invented"] = &typedChannel{}
			case "accounting":
				source.Accounting.RenderedBytes++
			case "rows":
				source.Channels["observations"].Items[0] = json.RawMessage(`{"revision":9223372036854775806,"text":"limit 7 界"}`)
			case "references":
				source.Retained = source.Retained[:1]
			}
			raw, _ := json.Marshal(source)
			result, err := ingressAssemble(ingressAssemblyRequest{Budget: 4096, TypedContextJSON: string(raw)})
			var refusal *contextBudgetError
			if result != nil || !errors.As(err, &refusal) || refusal.kind != "invalid_projection" {
				t.Fatal(result, err)
			}
		})
	}
}

func TestIngressTypedProjectionEmptyDegradedAndUnavailable(t *testing.T) {
	source := typedIngressFixture(t)
	source.degraded = true
	if err := source.fitProjectionBytes(0); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(source)
	code := []ingressCodeHit{{FilePath: "available.go"}}
	result, err := ingressAssemble(ingressAssemblyRequest{Budget: 1000, Code: code, TypedContextJSON: string(raw)})
	if err != nil {
		t.Fatal(err)
	}
	typed := result["typed_projection"].(map[string]any)
	if len(typed["retained_items"].([]typedProjectionRef)) != 0 || typed["context_sufficiency"] != "unknown" || strings.Contains(result["envelope"].(string), "temporal learning") || result["omitted_count"] != 0 {
		t.Fatal(result)
	}
	for _, failure := range []string{
		"", `{"status":"error","kind":"unavailable"}`,
		`{"status":"unavailable","dependency":"kb","retryable":true}`,
		`{"status":"stale","dependency":"kb"}`, `{"status":"unauthorized"}`,
		`{"status":"empty"}`, `{"status":"abstained"}`,
	} {
		result, err = ingressAssemble(ingressAssemblyRequest{Budget: 1000, Code: code, TypedRequested: true, TypedContextJSON: failure})
		if err != nil || result["typed_unavailable"] != true || !strings.Contains(result["envelope"].(string), "available.go") || result["typed_projection"] != nil {
			t.Fatal(result, err)
		}
	}
}

func TestIngressRefusesMalformedTypedOutcome(t *testing.T) {
	for _, raw := range []string{`{`, `{}`, `{"status":"invented"}`, `{"status":"ok"}`} {
		result, err := ingressAssemble(ingressAssemblyRequest{Budget: 1000, TypedRequested: true, TypedContextJSON: raw})
		var refusal *contextBudgetError
		if result != nil || !errors.As(err, &refusal) || refusal.kind != "invalid_projection" {
			t.Fatal("invalid projection was mistaken for optional unavailability", raw, result, err)
		}
	}
}

func TestIngressRejectsAmbiguousTypedInputs(t *testing.T) {
	source := typedIngressFixture(t)
	raw, _ := json.Marshal(source)
	for _, value := range []string{string(raw), `{"status":"error"}`} {
		result, err := ingressAssemble(ingressAssemblyRequest{TypedContextJSON: value, Temporal: "unbound legacy text"})
		if err == nil || result != nil {
			t.Fatal("conflicting typed inputs accepted", result, err)
		}
	}
}

func TestIngressPropagatesTypedOwnerRefusal(t *testing.T) {
	for _, kind := range []string{"protected_context_overflow", "context_budget_overflow", "unsupported_mode", "invalid_projection", "invalid_timestamp"} {
		for _, field := range []string{"kind", "error_type"} {
			for _, placement := range []Placement{PlacementServer, PlacementKB} {
				args := map[string]any{"operation": "ingress-assemble", "budget": 1000, "typed_requested": true,
					"code":               []ingressCodeHit{{FilePath: "available.go"}},
					"typed_context_json": fmt.Sprintf(`{"status":"error",%q:%q}`, field, kind)}
				raw, _ := json.Marshal(args)
				got := runHostRuntime(t, NewHandler(nil, WithDataStore(placement, nil)), string(raw))
				if got["status"] != "error" || got["kind"] != kind || got["envelope"] != nil {
					t.Fatalf("owner refusal omitted: %s %s %v", field, kind, got)
				}
			}
		}
	}
}
