package memory

import (
	"encoding/binary"
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestPublicCommandDiscovery(t *testing.T) {
	request := []byte{'D', 'C', 'M', 'D', 2, 0, 0, 0}
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		response, status := handler(bus.ModuleInvocation{StageID: bus.StageDescribeCommands}, request)
		if status != bus.ModuleStatusOK || len(response) < 16 {
			t.Fatalf("%s: %d %x", placement, status, response)
		}
		if stage := binary.LittleEndian.Uint32(response[12:]); stage != StageCommand {
			t.Fatal(stage)
		}
		count := int(binary.LittleEndian.Uint32(response[8:]))
		seen := map[string]bool{}
		offset := 16
		for i := 0; i < count; i++ {
			if offset+16 > len(response) {
				t.Fatal("truncated record")
			}
			surfaces := binary.LittleEndian.Uint32(response[offset:])
			groupLen, verbLen, summaryLen := int(binary.LittleEndian.Uint16(response[offset+8:])), int(binary.LittleEndian.Uint16(response[offset+10:])), int(binary.LittleEndian.Uint16(response[offset+12:]))
			offset += 16
			if offset+groupLen+verbLen+summaryLen > len(response) {
				t.Fatal("truncated strings")
			}
			group, verb := string(response[offset:offset+groupLen]), string(response[offset+groupLen:offset+groupLen+verbLen])
			offset += groupLen + verbLen + summaryLen
			wantGroup := "memory"
			if verb == "schema_list" {
				wantGroup = "relations"
			}
			switch verb {
			case "fold_session", "anti_pattern_extract_from_feedback", "anti_pattern_extract_from_failures", "anti_pattern_escalate", "memory_learn_style", "scan_conversations":
				wantGroup = "maintenance"
			}
			if group != wantGroup || (surfaces != SurfaceRPC && !((verb == "embed_text" || verb == "runtime") && surfaces == 0)) || seen[verb] {
				t.Fatalf("bad route %s.%s mask=%d", group, verb, surfaces)
			}
			seen[verb] = true
		}
		if placement == PlacementServer {
			if offset != len(response) || len(seen) != 4 || !seen["screen_content"] || !seen["pack"] {
				t.Fatal("private commands shadowed shared KB commands", seen)
			}
			continue
		}
		if offset != len(response) || len(seen) != 97 {
			t.Fatalf("routes=%d bytes=%d/%d", len(seen), offset, len(response))
		}
		for _, verb := range []string{"recall", "directive_create", "prospective_match", "list_unused_l2", "stats", "cognify", "cognify_drain", "cognify_status"} {
			if !seen[verb] {
				t.Fatal("missing public route", verb)
			}
		}
		for _, verb := range []string{"stats_dashboard", "review_console", "directive_briefing"} {
			if seen[verb] {
				t.Fatal("premature/internal route exposed", verb)
			}
		}
	}
}

// Retains the native schema/validation regressions at the enforcing Go owner.
func TestPublicEnforcedOntology(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	client := clientForHandler(t, handler)
	result := runPublicCommand(t, client, "schema_list", `{}`)
	if result["status"] != "ok" {
		t.Fatal(result)
	}
	rows := result["rows"].([]any)
	if len(rows) != 18 {
		t.Fatal("incomplete graph schema", len(rows))
	}
	foundFixes := false
	previous := [3]int{-1, -1, -1}
	for _, item := range rows {
		row := item.(map[string]any)
		order := [3]int{int(row["relation_id"].(float64)), int(row["subject_kind"].(float64)), int(row["object_kind"].(float64))}
		if order[0] < previous[0] || (order[0] == previous[0] && (order[1] < previous[1] || order[1] == previous[1] && order[2] < previous[2])) {
			t.Fatal("unstable schema ordering", row)
		}
		previous = order
		if row["relation"] == "fixes" && row["subject"] == "commit" && row["object"] == "bug" {
			foundFixes = true
			if order != [3]int{2, 5, 4} {
				t.Fatal(row)
			}
		}
	}
	if !foundFixes {
		t.Fatal("missing commit fixes bug schema")
	}
	for _, tt := range []struct {
		subject, relation, object int
		allowed                   bool
	}{
		{5, 2, 4, true}, {0, 12, 8, true}, {0, 99, 3, true}, {1, 2, 4, false},
	} {
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{Operation: "ontology-validate", SubjectKind: tt.subject, RelationCode: tt.relation, ObjectKind: tt.object}))
		var response DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(raw, &response) != nil || response.Allowed == nil || *response.Allowed != tt.allowed {
			t.Fatal(tt, string(raw), status)
		}
	}
}
