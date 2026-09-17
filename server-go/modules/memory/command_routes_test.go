package memory

import (
	"encoding/binary"
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
		if placement == PlacementServer {
			if count != 0 {
				t.Fatal("private commands shadowed the shared KB surface")
			}
			continue
		}
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
			if group != "memory" || surfaces != SurfaceRPC || seen[verb] {
				t.Fatalf("bad route %s.%s mask=%d", group, verb, surfaces)
			}
			seen[verb] = true
		}
		if offset != len(response) || len(seen) != 63 {
			t.Fatalf("routes=%d bytes=%d/%d", len(seen), offset, len(response))
		}
		for _, verb := range []string{"recall", "directive_create", "prospective_match", "list_unused_l2", "stats"} {
			if !seen[verb] {
				t.Fatal("missing public route", verb)
			}
		}
		for _, verb := range []string{"find_facts", "stats_dashboard", "review_console", "directive_briefing"} {
			if seen[verb] {
				t.Fatal("premature/internal route exposed", verb)
			}
		}
	}
}
