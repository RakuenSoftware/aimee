package memory

import (
	"context"
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

type nativeRestrictedCommandStore struct {
	commandStore
	restriction SearchRestriction
	calls       int
}

func (s *nativeRestrictedCommandStore) SearchRestricted(_ context.Context, scope Scope, query, kind, tier string, limit int, restriction SearchRestriction) ([]Record, error) {
	s.scope, s.query, s.limit, s.restriction = scope, query, limit, restriction
	s.calls++
	return []Record{{ID: 6, Scope: scope, Content: "released fact"}}, nil
}
func TestPrivateSearchRestrictionReachesOwnerAndReceipt(t *testing.T) {
	s := &nativeRestrictedCommandStore{}
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, s)))
	restriction := SearchRestriction{Version: 1, Namespace: "user-snapshot", Authorization: strings.Repeat("a", 64), IDs: []int64{6}}
	raw, _ := json.Marshal(map[string]any{"keywords": []string{"released fact"}, "limit": 3, "restriction": restriction, "scope": map[string]string{"type": "global", "value": "_global"}})
	result := runPublicCommand(t, client, "search", string(raw))
	if result["status"] != "ok" || s.calls != 1 || s.scope != (Scope{Type: ScopeUser, Value: "_user"}) || !reflect.DeepEqual(s.restriction, restriction) || s.limit != 3 {
		t.Fatalf("restriction not enforced: %+v %+v", result, s)
	}
	encoded, _ := json.Marshal(result["restriction"])
	var receipt SearchRestriction
	if json.Unmarshal(encoded, &receipt) != nil || !reflect.DeepEqual(receipt, restriction) {
		t.Fatal("missing enforced receipt")
	}
	for _, invalid := range []string{"null", `{"version":1}`, `{"version":1,"namespace":"user","authorization":"` + strings.Repeat("a", 64) + `","ids":[6,6]}`} {
		result = runPublicCommand(t, client, "search", `{"keywords":["released"],"restriction":`+invalid+`}`)
		if result["status"] != "error" || s.calls != 1 {
			t.Fatal("invalid restriction reached search")
		}
	}
	// An owner without this operation must deny, never fall back to ordinary search.
	unsupported := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementServer, &commandStore{})))
	if runPublicCommand(t, unsupported, "search", string(raw))["status"] != "error" {
		t.Fatal("unsupported owner ignored restriction")
	}
}
