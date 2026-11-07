package memory

import (
	"encoding/json"
	"fmt"
)

type executionGradeSnapshot struct {
	CheckID   string            `json:"check_id,omitempty"`
	Schema    int               `json:"schema_version"`
	Principal string            `json:"principal"`
	ScopeKind string            `json:"scope_kind"`
	ScopeID   string            `json:"scope_id"`
	Coverage  string            `json:"coverage"`
	Items     []json.RawMessage `json:"items"`
}

func retainedExecutionGrades(raw json.RawMessage, selected []int, count int) json.RawMessage {
	if count == 0 {
		return nil
	}
	var snapshot executionGradeSnapshot
	if json.Unmarshal(raw, &snapshot) != nil {
		return nil
	}
	items := []json.RawMessage{}
	for _, index := range selected {
		if index < count {
			items = append(items, snapshot.Items[index])
		}
	}
	if len(items) == 0 {
		return nil
	}
	snapshot.Items = items
	body, _ := json.Marshal(snapshot)
	return body
}

func mergeExecutionGradeSources(previous, current json.RawMessage) (json.RawMessage, error) {
	if len(current) == 0 {
		return previous, nil
	}
	if len(previous) == 0 {
		return current, nil
	}
	var a, b executionGradeSnapshot
	if json.Unmarshal(previous, &a) != nil || json.Unmarshal(current, &b) != nil || a.Principal != b.Principal || a.ScopeID != b.ScopeID {
		return nil, fmt.Errorf("test receipt audience changed")
	}
	seen := map[string]bool{}
	items := []json.RawMessage{}
	for _, raw := range append(a.Items, b.Items...) {
		var item any
		if json.Unmarshal(raw, &item) != nil {
			return nil, fmt.Errorf("invalid test receipt")
		}
		digest := releaseDigest(item)
		if !seen[digest] {
			seen[digest] = true
			items = append(items, raw)
		}
	}
	a.Items = items
	raw, err := json.Marshal(a)
	if len(raw) > 60*1024 || len(items) > 64 {
		return nil, fmt.Errorf("test receipt source capacity")
	}
	return raw, err
}

func executionGradeSourcesEqual(a, b json.RawMessage) bool {
	var x, y executionGradeSnapshot
	if json.Unmarshal(a, &x) != nil || json.Unmarshal(b, &y) != nil || x.Schema != y.Schema || x.Principal != y.Principal || x.ScopeKind != y.ScopeKind || x.ScopeID != y.ScopeID || x.Coverage != y.Coverage || len(x.Items) != len(y.Items) {
		return false
	}
	values := map[string]bool{}
	for _, raw := range x.Items {
		var item any
		if json.Unmarshal(raw, &item) != nil {
			return false
		}
		values[releaseDigest(item)] = true
	}
	if len(values) != len(x.Items) {
		return false
	}
	for _, raw := range y.Items {
		var item any
		if json.Unmarshal(raw, &item) != nil || !values[releaseDigest(item)] {
			return false
		}
	}
	return true
}
