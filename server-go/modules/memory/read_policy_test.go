package memory

import (
	"encoding/json"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestMemoryReadPolicyRejectsUnimplementedSemantics(t *testing.T) {
	for _, tc := range []struct {
		name, operation, asOf, want string
		placement                   Placement
		policy                      MemoryReadPolicy
	}{
		{"future schema", "get", "", "unsupported_version", PlacementKB, MemoryReadPolicy{SchemaVersion: 2, Mode: "current"}},
		{"missing schema", "get", "", "unsupported_version", PlacementKB, MemoryReadPolicy{Mode: "current"}},
		{"belief time", "get", "", "unsupported_mode", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical", ValidAt: "2026-01-01", BelievedAt: "2026-02-01"}},
		{"diagnostic", "get", "", "unsupported_mode", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "diagnostic"}},
		{"search", "search", "", "unsupported_mode", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical", ValidAt: "2026-01-01"}},
		{"mutation", "put", "", "unsupported_mode", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "current"}},
		{"legacy mixed", "get", "2026-01-01", "invalid_argument", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical", ValidAt: "2026-01-01"}},
		{"current override", "get", "", "invalid_argument", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "current", ValidAt: "2026-01-01"}},
		{"missing time", "get", "", "invalid_argument", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical"}},
		{"relative time", "get", "", "invalid_argument", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical", ValidAt: "now"}},
		{"bad calendar", "get", "", "invalid_argument", PlacementKB, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical", ValidAt: "2026-02-30"}},
		{"personal history", "get", "", "unsupported_mode", PlacementServer, MemoryReadPolicy{SchemaVersion: 1, Mode: "historical", ValidAt: "2026-01-01"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			// No datastore: validation must precede any read or mutation.
			raw, status := handleData(handlerOptions{placement: tc.placement}, bus.ModuleInvocation{}, dataRequest(t, DataRequest{
				Operation: tc.operation, ID: 42, AsOf: tc.asOf, ReadPolicy: &tc.policy,
			}))
			var got DataResponse
			if status != bus.ModuleStatusOK || json.Unmarshal(raw, &got) != nil || got.Read == nil || got.Read.ErrorCode != tc.want || len(got.Records) != 0 {
				t.Fatalf("status=%v response=%s", status, raw)
			}
		})
	}
}

func TestMemoryReadPolicyPublicErrors(t *testing.T) {
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		client := clientForHandler(t, NewHandler(nil, WithDataStore(placement, nil)))
		for _, tc := range []struct{ verb, args, want string }{
			{"get", `{"id":42,"read_policy":{"schema_version":3,"mode":"current"}}`, "unsupported_version"},
			{"get", `{"id":42,"read_policy":{"schema_version":1,"mode":"current","believed_at":"2026-01-01"}}`, "unsupported_mode"},
			{"get", `{"id":42,"read_policy":null}`, "invalid_argument"},
			{"get", `{"id":42,"read_policy":{"schema_version":1,"mode":"current","principal":"forged"}}`, "invalid_argument"},
			{"get", `{"id":42,"read_policy":{"schema_version":1,"mode":"current","include_all":true}}`, "invalid_argument"},
			{"search", `{"query":"test","read_policy":{"schema_version":1,"mode":"historical","valid_at":"2026-01-01"}}`, "unsupported_mode"},
			{"delete", `{"id":42,"read_policy":{"schema_version":1,"mode":"current"}}`, "unsupported_mode"},
		} {
			got := runPublicCommand(t, client, tc.verb, tc.args)
			if got["kind"] != tc.want || got["memory"] != nil {
				t.Fatalf("placement=%s verb=%s args=%s result=%v", placement, tc.verb, tc.args, got)
			}
		}
	}
}
