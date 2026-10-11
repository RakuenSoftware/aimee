package memory

import (
	"context"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/memory/backendstore"
	"path/filepath"
	"testing"
)

type externalTestBackend struct {
	contract.Store
	name string
}

func (b externalTestBackend) Capabilities() contract.Capability {
	return contract.Capability{Name: b.name, Version: 1}
}

func TestExternalRecallCompositionAndSourceRelease(t *testing.T) {
	for _, engine := range []string{"cognee", "hillock"} {
		t.Run(engine, func(t *testing.T) {
			ctx := context.Background()
			create := func(placement Placement) *externalDataStore {
				c, e := backendstore.New(filepath.Join(t.TempDir(), engine), "11111111-1111-4111-8111-111111111111")
				if e != nil {
					t.Fatal(e)
				}
				b := externalTestBackend{c, engine}
				return &externalDataStore{ContractDataStore: ContractDataStore{b}, catalog: c, backend: b, placement: placement}
			}
			kb := create(PlacementKB)
			personal := create(PlacementServer)
			scope := Scope{Type: ScopeProject, Value: "discord"}
			foreign := Scope{Type: ScopeProject, Value: "foreign"}
			put := func(s *externalDataStore, scope Scope, key, content, kind string) Record {
				r, e := s.catalog.Put(ctx, scope, contract.Record{Kind: kind, Tier: "L2", Key: key, Content: content, Confidence: 1})
				if e != nil {
					t.Fatal(e)
				}
				out, e := fromContractRecord(r)
				if e != nil {
					t.Fatal(e)
				}
				return out
			}
			put(kb, scope, "identity:Kibukx", "Kibukx's height is 69cm; source Virant message 1", "fact")
			put(kb, foreign, "identity:Kibukx", "500 pounds", "fact")
			put(kb, Scope{Type: ScopeWorkspace, Value: "_shared"}, "preference:response", "short replies", "preference")
			put(personal, Scope{Type: ScopeUser, Value: "_user"}, "preference:response", "explain evidence", "preference")
			invoke := func(s *externalDataStore, r DataRequest) DataResponse {
				raw, _ := json.Marshal(r)
				result, status := handleData(handlerOptions{placement: s.placement, data: s}, bus.ModuleInvocation{}, raw)
				if status != bus.ModuleStatusOK {
					t.Fatalf("%s status=%d", r.Operation, status)
				}
				var out DataResponse
				if json.Unmarshal(result, &out) != nil {
					t.Fatal(string(result))
				}
				return out
			}
			recalled := invoke(kb, DataRequest{Operation: "recall-bundle", Query: "height", Project: "discord", LimitTokens: 8192})
			var bundle recallBundle
			if json.Unmarshal(recalled.Payload, &bundle) != nil || len(bundle.Identity) != 1 || len(bundle.Preferences) != 1 {
				t.Fatal(string(recalled.Payload))
			}
			if bundle.Identity[0].Content != "Kibukx's height is 69cm; source Virant message 1" {
				t.Fatal(bundle.Identity)
			}
			envelope, _ := json.Marshal(map[string]any{"status": "ok", "recall": bundle})
			composed := invoke(personal, DataRequest{Operation: "compose-recall", SharedRecall: envelope, LimitTokens: 8192})
			var out struct {
				Recall recallBundle `json:"recall"`
			}
			if json.Unmarshal(composed.Payload, &out) != nil || len(out.Recall.Preferences) != 1 || out.Recall.Preferences[0].Content != "explain evidence" || out.Recall.PersonalCollection == nil {
				t.Fatal(string(composed.Payload))
			}
			briefing := invoke(kb, DataRequest{Operation: "briefing-bundle", Project: "discord", LimitTokens: 8192})
			var b briefingBundle
			if json.Unmarshal(briefing.Payload, &b) != nil || len(b.Facts) != 2 {
				t.Fatal(string(briefing.Payload))
			}
			refs := []typedProjectionRef{{Channel: "native_memory_collection", ID: "1", Source: bundle.CollectionSource}}
			req := &sourceRevalidation{SchemaVersion: 1, CheckID: "11111111111111111111111111111111", Sources: refs}
			if !req.valid() {
				t.Fatal("invalid collection source", req)
			}
			audience := externalAudience(scope, "")
			if ok, e := kb.externalRevalidate(ctx, req, audience); e != nil || !ok {
				t.Fatal(ok, e)
			}
			put(kb, scope, "correction", "170cm", "fact")
			if ok, e := kb.externalRevalidate(ctx, req, audience); e != nil || ok {
				t.Fatal("stale source released", ok, e)
			}
		})
	}
}
