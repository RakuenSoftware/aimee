package memory

import (
	"context"
	"encoding/json"
	"sort"
	"strings"

	contract "github.com/JBailes/aimee/server-go/memory"
)

func externalAudience(scope Scope, workspace string) []Scope {
	result := []Scope{scope, {Type: ScopeGlobal, Value: "_global"}, {Type: ScopeWorkspace, Value: "_shared"}}
	if workspace != "" {
		result = append(result, Scope{Type: ScopeWorkspace, Value: workspace})
	}
	sort.Slice(result, func(i, j int) bool {
		return result[i].Type+"\x00"+result[i].Value < result[j].Type+"\x00"+result[j].Value
	})
	out := []Scope{}
	for _, item := range result {
		if len(out) == 0 || out[len(out)-1] != item {
			out = append(out, item)
		}
	}
	return out
}

type externalSearch func(string, string, string, int) ([]contract.Record, error)

func (s *externalDataStore) collectionSource(ctx context.Context, scopes []Scope) (*typedSourceVersion, error) {
	version, err := s.catalog.Observe(ctx)
	if err != nil {
		return nil, err
	}
	deadline, err := s.catalog.Boundary(ctx, scopes)
	if err != nil {
		return nil, err
	}
	kind := "memory_collection"
	if s.placement == PlacementServer {
		kind = "user_memory_collection"
	}
	source := &typedSourceVersion{Kind: kind, Version: version, MemoryParentState: "observed", CollectionValidUntil: deadline}
	if s.placement == PlacementKB {
		source.CollectionAudience = scopes
	}
	return source, nil
}

func externalRecords(records []contract.Record) ([]Record, error) {
	out := []Record{}
	for _, item := range records {
		r, err := fromContractRecord(item)
		if err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, nil
}

func (s *externalDataStore) externalBundle(ctx context.Context, req DataRequest, scopes []Scope, search externalSearch) (json.RawMessage, error) {
	if req.Operation == "compose-recall" && s.placement != PlacementServer {
		return nil, contract.ErrClientRequest
	}
	if req.Operation == "briefing-bundle" && s.placement != PlacementKB {
		return nil, contract.ErrClientRequest
	}
	// Capture before selection. A concurrent mutation makes the source check
	// refuse the entire prepared projection, including an empty result.
	source, err := s.collectionSource(ctx, scopes)
	if err != nil {
		return nil, err
	}
	raw, err := search("", "", "", 256)
	if err != nil {
		return nil, err
	}
	records, err := externalRecords(raw)
	if err != nil {
		return nil, err
	}
	if req.Operation == "briefing-bundle" {
		b := briefingBundle{Facts: []briefingFact{}, Activity: []briefingActivity{}, Entities: []briefingEntity{}, Style: "compact", BriefingStyle: "compact", LimitTokens: recallTokenLimit(req.LimitTokens, true)}
		for _, r := range recallItems(records) {
			if r.Kind != "scratch" {
				b.Facts = append(b.Facts, briefingFact{RecallRecord: r})
			}
		}
		return b.encodeBudgeted()
	}
	b := recallBundle{CollectionSource: source, AlwaysOnRules: []recallRule{}, Identity: []RecallRecord{}, Preferences: []RecallRecord{}, ActiveContext: []RecallRecord{}, OpenCommitments: []RecallRecord{}, Reminders: []recallReminder{}, Directives: []recallDirective{}, Explain: []any{}, LimitTokens: recallTokenLimit(req.LimitTokens, req.SessionStart), SessionStart: req.SessionStart}
	for _, r := range recallItems(records) {
		if r.Kind == "preference" {
			b.Preferences = append(b.Preferences, r)
		}
		for _, prefix := range []string{"identity:", "name:", "role:", "user:", "self:"} {
			if r.Kind == "fact" && strings.HasPrefix(r.Key, prefix) {
				b.Identity = append(b.Identity, r)
				break
			}
		}
	}
	if req.Operation == "compose-recall" {
		var envelope map[string]json.RawMessage
		if len(req.SharedRecall) == 0 || len(req.SharedRecall) > maxDataBody/2 || json.Unmarshal(req.SharedRecall, &envelope) != nil || envelope == nil {
			return nil, contract.ErrClientRequest
		}
		var status string
		if json.Unmarshal(envelope["status"], &status) != nil {
			return nil, contract.ErrClientRequest
		}
		if status != "ok" {
			if status == "error" || status == "degraded" || status == "quarantined" {
				return req.SharedRecall, nil
			}
			return nil, contract.ErrClientRequest
		}
		var shared recallBundle
		if json.Unmarshal(envelope["recall"], &shared) != nil || shared.Identity == nil || shared.Preferences == nil || shared.ActiveContext == nil || shared.OpenCommitments == nil || shared.AlwaysOnRules == nil || shared.Reminders == nil || shared.Directives == nil {
			return nil, contract.ErrClientRequest
		}
		shared.PersonalCollection = source
		personalIdentity := []Record{}
		for _, r := range b.Identity {
			personalIdentity = append(personalIdentity, r.Record)
		}
		personalPreferences := []Record{}
		for _, r := range b.Preferences {
			personalPreferences = append(personalPreferences, r.Record)
		}
		shared.Identity = composeRecallSection(personalIdentity, shared.Identity, "user identity")
		shared.Preferences = composeRecallSection(personalPreferences, shared.Preferences, "user preference")
		shared.LimitTokens = b.LimitTokens
		shared.SessionStart = b.SessionStart
		body, e := shared.encodeBudgeted()
		if e != nil {
			return nil, e
		}
		envelope["recall"] = body
		envelope["store"] = json.RawMessage(`"composed"`)
		return json.Marshal(envelope)
	}
	active, err := search(req.Query, "", "", min(32, max(5, req.Limit)))
	if err != nil {
		return nil, err
	}
	converted, err := externalRecords(active)
	if err != nil {
		return nil, err
	}
	b.ActiveContext = recallItems(converted)
	// Optional native-only graph, reminder and learning sections remain empty;
	// provider text never becomes an authoritative instruction or derived fact.
	return b.encodeBudgeted()
}

func (s *externalDataStore) externalRevalidate(ctx context.Context, req *sourceRevalidation, scopes []Scope) (bool, error) {
	var collection *MemoryRecordVersion
	versions := []MemoryRecordVersion{}
	boundaries := []string{}
	for _, ref := range req.Sources {
		source := ref.Source
		boundaries = append(boundaries, source.CollectionValidUntil)
		private := s.placement == PlacementServer
		switch source.Kind {
		case "memory_collection", "user_memory_collection":
			if (source.Kind == "user_memory_collection") != private {
				return false, nil
			}
			if !private {
				raw, _ := json.Marshal(scopes)
				observed, _ := json.Marshal(source.CollectionAudience)
				if string(raw) != string(observed) {
					return false, nil
				}
			}
			copy := source.Version
			collection = &copy
		case "memory_record", "user_memory_record":
			if (source.Kind == "user_memory_record") != private {
				return false, nil
			}
			versions = append(versions, source.Version)
		default:
			return false, contract.ErrUnsupported
		}
	}
	return s.catalog.Revalidate(ctx, req.CheckID, req.SendGuard, collection, scopes, versions, boundaries...)
}
