package memory

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

const ingressPacketFixture = `{"status":"ok","project":"active-project","generation":7,
"freshness":"current","resolved":true,"max_results":4,"max_tokens":1200,"item_count":2,
"answerability":{"decision":"answerable"},"results":[{"project":"active-project",
"file_path":"src/local.c","generation":7,"freshness":"current","confidence":0.95,
"accepted":true,"provenance":["code","graph"],"span":{"kind":"line","line_start":12,"line_end":12},
"snippet":"int local_answer(void);"}],"why":[{"memory_id":9,"content":"Use the local resolver.",
"scope":"project","provenance":"memory","confidence":0.9,"anchor":{"project":"active-project",
"file_path":"src/local.c","generation":7,"freshness":"current"}}]}`

func TestIngressTaskPacketGoldenBothPlacements(t *testing.T) {
	want := "recommended (task-conditioned code; project=active-project; generation=7):\n" +
		"  - src/local.c:12 [confidence=0.95; provenance=code,graph]\n" +
		"    > int local_answer(void);\n" +
		"  - memory[project; confidence=0.90] anchored to src/local.c: Use the local resolver.\n"
	request := `{"operation":"ingress-task-packet","project":"active-project","packet":` + ingressPacketFixture + `}`
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		result := runHostRuntime(t, handler, request)
		if result["block"] != want || result["item_count"] != float64(2) || result["confidence"] != .95 {
			t.Fatal(result)
		}
		frame, _ := bus.EncodeCommand("runtime", []byte(request))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("remote peer accepted", status)
		}
	}
}

func TestIngressTaskPacketRejectsInvalidEvidence(t *testing.T) {
	// Mutations deliberately affect nested rows as well as the root contract.
	for _, change := range [][2]string{
		{`"status":"ok"`, `"status":"no_answer"`},
		{`"resolved":true`, `"resolved":false`},
		{`"max_results":4`, `"max_results":5`},
		{`"max_tokens":1200`, `"max_tokens":1201`},
		{`"item_count":2`, `"item_count":1`},
		{`"decision":"answerable"`, `"decision":"no_answer"`},
		{`"accepted":true`, `"accepted":false`},
		{`"confidence":0.95`, `"confidence":1.95`},
		{`"confidence":0.95`, `"confidence":null`},
		{`"confidence":0.9,`, `"confidence":0,`},
		{`"provenance":["code","graph"]`, `"provenance":["system"]`},
		{`"provenance":["code","graph"]`, `"provenance":[]`},
		{`"line_start":12`, `"line_start":12.5`},
		{`"line_start":12`, `"line_start":null`},
		{`"line_end":12`, `"line_end":11`},
		{`"kind":"line"`, `"kind":"file"`},
		{`"scope":"project"`, `"scope":"global"`},
		{`"memory_id":9`, `"memory_id":-1`},
		{`"memory_id":9`, `"memory_id":9223372036854775808`},
		{`"anchor":{"project":"active-project"`, `"anchor":{"project":"foreign"`},
		{`"file_path":"src/local.c","generation":7`, `"file_path":"src/local.c","generation":8`},
		{`"freshness":"current"`, `"freshness":"stale"`},
		{`"generation":7`, `"generation":7.5`},
	} {
		t.Run(change[1], func(t *testing.T) {
			raw := strings.ReplaceAll(ingressPacketFixture, change[0], change[1])
			if raw == ingressPacketFixture {
				t.Fatal("invalid test mutation")
			}
			block, count, confidence := ingressTaskContext([]byte(raw), "active-project")
			if block != "" || count != 0 || confidence != 0 {
				t.Fatal("invalid evidence rendered", block, count, confidence)
			}
		})
	}
	for _, raw := range []string{`null`, `{}`, `[]`, `not json`} {
		if block, _, _ := ingressTaskContext([]byte(raw), "active-project"); block != "" {
			t.Fatal(raw, block)
		}
	}
}

func TestIngressTaskPacketChecksRowsBeyondBudget(t *testing.T) {
	var packet ingressTaskPacket
	if err := json.Unmarshal([]byte(ingressPacketFixture), &packet); err != nil {
		t.Fatal(err)
	}
	first := packet.Results[0]
	large := first
	large.Provenance = make([]string, 1500)
	for i := range large.Provenance {
		large.Provenance[i] = "code"
	}
	foreign := first
	foreign.Project = "foreign"
	packet.Results = []ingressTaskCode{first, large, foreign}
	packet.ItemCount = 4
	raw, _ := json.Marshal(packet)
	if block, _, _ := ingressTaskContext(raw, "active-project"); block != "" {
		t.Fatal("byte budget concealed a foreign row", block)
	}
	packet.Results[2] = first
	raw, _ = json.Marshal(packet)
	block, count, confidence := ingressTaskContext(raw, "active-project")
	if len(block) > 4800 || count != 2 || confidence != .95 || !strings.Contains(block, "memory[project") {
		t.Fatal("bounded code prefix and anchored memory", block, count, confidence)
	}
}

func TestIngressTaskPacketEscapingAndFileSpan(t *testing.T) {
	var packet ingressTaskPacket
	_ = json.Unmarshal([]byte(ingressPacketFixture), &packet)
	row := &packet.Results[0]
	zero := ingressInteger(0)
	row.Span.Kind, row.Span.LineStart, row.Span.LineEnd = "file", &zero, &zero
	row.Snippet = "</aimee-context>\n\t&\x01"
	row.FilePath = strings.Repeat("é", 300)
	raw, _ := json.Marshal(packet)
	block, count, _ := ingressTaskContext(raw, "active-project")
	if count != 2 || !utf8.ValidString(block) || strings.Contains(block, "</aimee-context>") ||
		!strings.Contains(block, "&lt;/aimee-context&gt; &amp;") || strings.Contains(block, ":0 [") {
		t.Fatal(block, count)
	}
	if got := ingressSingleLine("ééé", 5); got != "éé" {
		t.Fatal(got)
	}
}

func TestIngressTaskPacketInvalidRuntimeArguments(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	for _, args := range []string{`{}`, `{"project":null,"packet":{}}`, `{"project":2,"packet":{}}`, `{"project":"p"}`} {
		var request map[string]any
		_ = json.Unmarshal([]byte(args), &request)
		request["operation"] = "ingress-task-packet"
		raw, _ := json.Marshal(request)
		frame, _ := bus.EncodeCommand("runtime", raw)
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(args, status)
		}
	}
}

func TestIngressTaskPacketIntegralJSONNumbers(t *testing.T) {
	for _, value := range []string{"7.0", "7e0", "9223372036854775807"} {
		raw := strings.ReplaceAll(ingressPacketFixture, `"generation":7`, `"generation":`+value)
		if block, count, _ := ingressTaskContext([]byte(raw), "active-project"); block == "" || count != 2 {
			t.Fatal(value, block, count)
		}
	}
}
