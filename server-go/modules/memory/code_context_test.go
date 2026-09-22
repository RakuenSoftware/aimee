package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

const codeContextInput = `{"query":"needle","project":"app","generation":9223372036854775807,"hybrid":{"results":[{"project":"foreign","file_path":"secret.go","signals":["code"]},{"project":"app","file_path":"weak.go","signals":["vector"],"vector_score":0.69},{"project":"app","file_path":"vector.go","signals":["vector"],"vector_score":0.8},{"project":"app","file_path":"main.go","signals":["code"],"snippet":"func lookup() {}","caller":"lookup"}]},"enriched":[{"project":"app","file_path":"main.go","line":17}]}`

func codeContextFixture(t *testing.T) codeContextRequest {
	t.Helper()
	var request codeContextRequest
	if err := json.Unmarshal([]byte(codeContextInput), &request); err != nil {
		t.Fatal(err)
	}
	return request
}
func TestCodeContextAdmissionAndAnchors(t *testing.T) {
	request := codeContextFixture(t)
	memory := codeContextMemory{publicMemoryRecord: publicMemoryRecord{ID: 9223372036854775807, Kind: "decision", Content: "main.go: needle uses stable lookup", Confidence: .8}, Scope: Scope{ScopeProject, "app"}}
	request.Memories = []codeContextMemory{memory}
	result := buildCodeContext(request)
	raw, _ := json.Marshal(result.Packet)
	if result.HTTPStatus != 200 || strings.Contains(string(raw), "secret.go") || strings.Contains(string(raw), "weak.go") || !strings.Contains(string(raw), `"generation":9223372036854775807`) || !strings.Contains(string(raw), `"memory_id":9223372036854775807`) {
		t.Fatal(string(raw))
	}
	packet := result.Packet.(map[string]any)
	rows := packet["results"].([]map[string]any)
	if len(rows) != 2 || rows[0]["file_path"] != "main.go" || rows[1]["file_path"] != "vector.go" || packet["item_count"] != 3 {
		t.Fatal(packet)
	}
	if rows[0]["span"].(map[string]any)["line_start"] != ingressInteger(17) {
		t.Fatal(rows[0])
	}
	for name, change := range map[string]func(*codeContextMemory){
		"global":           func(m *codeContextMemory) { m.Scope = Scope{ScopeGlobal, "_global"} },
		"foreign":          func(m *codeContextMemory) { m.Scope.Value = "other" },
		"fact":             func(m *codeContextMemory) { m.Kind = "fact" },
		"unanchored":       func(m *codeContextMemory) { m.Content = "needle uses stable lookups" },
		"symbol substring": func(m *codeContextMemory) { m.Content = "needles use lookups" },
		"duplicate":        func(m *codeContextMemory) { m.Content = "main.go: " + strings.Repeat("func lookup() {}", 3) },
	} {
		t.Run(name, func(t *testing.T) {
			m := memory
			change(&m)
			request.Memories = []codeContextMemory{m}
			r := buildCodeContext(request).Packet.(map[string]any)
			if len(r["why"].([]map[string]any)) != 0 {
				t.Fatal(r)
			}
		})
	}
	for _, field := range []string{"key", "headline", "content", "use_cases"} {
		m := memory
		m.Content = "needle explanation"
		switch field {
		case "key":
			m.Key = "lookup rationale"
		case "headline":
			m.Headline = "why lookup"
		case "content":
			m.Content = "use lookup now"
		case "use_cases":
			m.UseCases = "when calling lookup"
		}
		request.Memories = []codeContextMemory{m}
		if r := buildCodeContext(request).Packet.(map[string]any); len(r["why"].([]map[string]any)) != 1 {
			t.Fatal(field, r)
		}
	}
	request.Hybrid.Results = nil
	request.Memories = []codeContextMemory{memory}
	if r := buildCodeContext(request).Packet.(map[string]any); r["status"] != "abstained" || r["item_count"] != 0 {
		t.Fatal(r)
	}
}

func TestCodeContextDependencyAndNearMisses(t *testing.T) {
	request := codeContextFixture(t)
	request.Hybrid.Results = request.Hybrid.Results[:2]
	for status, want := range map[string]int{"unavailable": 503, "stale": 409, "unauthorized": 401} {
		request.Hybrid.VectorStatus = status
		result := buildCodeContext(request)
		packet := result.Packet.(map[string]any)
		if result.HTTPStatus != want || packet["status"] != status || packet["dependency"] != "embedder" || packet["retryable"] != (status == "unavailable") {
			t.Fatal(result)
		}
	}
	request.Hybrid.VectorStatus = "ready"
	packet := buildCodeContext(request).Packet.(map[string]any)
	if packet["status"] != "abstained" {
		t.Fatal(packet)
	}
	near := packet["answerability"].(map[string]any)["near_misses"].([]map[string]any)
	if len(near) != 1 || near[0]["file_path"] != "weak.go" {
		t.Fatal(near)
	}
	request = codeContextFixture(t)
	for len(request.Hybrid.Results) < 10 {
		request.Hybrid.Results = append(request.Hybrid.Results, request.Hybrid.Results[3])
	}
	packet = buildCodeContext(request).Packet.(map[string]any)
	if packet["item_count"] != 4 || len(packet["why"].([]map[string]any)) != 0 {
		t.Fatal(packet)
	}
}

func TestCodeContextHostBoundary(t *testing.T) {
	for _, operation := range []string{"code-context", "code-context-plan"} {
		var args commandArgs
		json.Unmarshal([]byte(codeContextInput), &args)
		args["operation"], _ = json.Marshal(operation)
		encoded, _ := json.Marshal(args)
		frame, _ := bus.EncodeCommand("runtime", encoded)
		handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		r := runHostRuntime(t, handler, string(encoded))
		packet := r["packet_json"].(string)
		if !strings.Contains(packet, `"generation":9223372036854775807`) || r["http_status"] != float64(200) {
			t.Fatal(r)
		}
		for _, generation := range []string{"null", "1.5", "0", "9223372036854775808"} {
			args["generation"] = json.RawMessage(generation)
			raw, _ := json.Marshal(args)
			f, _ := bus.EncodeCommand("runtime", raw)
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, f); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(generation, status)
			}
		}
	}
}

func exerciseCodeContextReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	content := strings.Repeat("needle explanation ", 180) + " main.go"
	for _, project := range []string{"app", "foreign"} {
		if _, err := tx.Exec(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence,use_cases)
   VALUES('L2','decision',$1,$2,'project',$3,.8,'lookup rationale')`, "context-"+project, content, project); err != nil {
			t.Fatal(err)
		}
	}
	var args map[string]any
	json.Unmarshal([]byte(codeContextInput), &args)
	// Preserve the exact numeric generation from the source when marshaling.
	args["generation"] = json.Number("9223372036854775807")
	args["operation"] = "code-context"
	args["memories"] = []any{map[string]any{"id": 12, "kind": "decision", "content": "main.go FORGED", "scope": map[string]any{"type": "project", "value": "app"}}}
	raw, _ := json.Marshal(args)
	result := runHostRuntime(t, handler, string(raw))
	var packet struct {
		Why []struct {
			Content string `json:"content"`
			ID      int64  `json:"memory_id"`
		} `json:"why"`
	}
	if err := json.Unmarshal([]byte(result["packet_json"].(string)), &packet); err != nil {
		t.Fatal(err)
	}
	if len(packet.Why) != 1 || packet.Why[0].Content != content || packet.Why[0].ID == 12 {
		t.Fatal(fmt.Sprint(packet))
	}
}
