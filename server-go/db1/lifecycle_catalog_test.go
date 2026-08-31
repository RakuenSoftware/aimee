package db1

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// lifecycle_gen.go was generated from the store's operation catalog by
// scripts/gen_db1_contract.py. That generator went with the C store, so nothing
// regenerates this file any more and its own header can no longer be obeyed.
//
// This is what replaces it. The file is now maintained by hand, and hand
// maintenance without a check is how a client and its wire drift: an operation
// renumbered or removed in the catalog leaves these constants pointing at a
// different op than the one they name, which the module answers perfectly well
// and wrongly.
//
// Names are compared with separators stripped and case folded, because the
// generator wrote Go initialisms -- work_item_id_by_pr_ref became
// opWorkItemIDByPRRef, which no plain snake-to-camel rule reproduces.

type catalogOperation struct {
	Family string `json:"family"`
	ID     uint32 `json:"id"`
	Name   string `json:"name"`
}

var opConstRe = regexp.MustCompile(`(?m)^const (op\w+) uint32 = (\d+)$`)

func foldName(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z', r >= '0' && r <= '9':
			b.WriteRune(r)
		case r >= 'A' && r <= 'Z':
			b.WriteRune(r + ('a' - 'A'))
		}
	}
	return b.String()
}

func loadLifecycleCatalog(t *testing.T) map[string]uint32 {
	t.Helper()
	path := filepath.Join("..", "modules", "aimee", "operations.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("catalog not readable at %s: %v", path, err)
	}
	var doc struct {
		Operations []catalogOperation `json:"operations"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("catalog is not valid JSON: %v", err)
	}
	byName := map[string]uint32{}
	for _, op := range doc.Operations {
		if op.Family == "lifecycle" {
			byName[foldName(op.Name)] = op.ID
		}
	}
	if len(byName) == 0 {
		t.Fatalf("the catalog declares no lifecycle operations")
	}
	return byName
}

func TestLifecycleOpConstantsMatchTheCatalog(t *testing.T) {
	catalog := loadLifecycleCatalog(t)

	src, err := os.ReadFile("lifecycle_gen.go")
	if err != nil {
		t.Fatalf("lifecycle_gen.go: %v", err)
	}
	matches := opConstRe.FindAllStringSubmatch(string(src), -1)
	if len(matches) == 0 {
		t.Fatalf("no op constants found; the declaration shape must have changed")
	}

	seen := map[string]bool{}
	for _, m := range matches {
		name := foldName(strings.TrimPrefix(m[1], "op"))
		var id uint32
		for _, c := range m[2] {
			id = id*10 + uint32(c-'0')
		}
		seen[name] = true
		want, ok := catalog[name]
		if !ok {
			t.Errorf("%s is declared here but the catalog has no such lifecycle op", m[1])
			continue
		}
		if want != id {
			t.Errorf("%s = %d, the catalog numbers it %d", m[1], id, want)
		}
	}

	for name, id := range catalog {
		if !seen[name] {
			t.Errorf("lifecycle op %d (%s) is in the catalog but has no constant here", id, name)
		}
	}
}

func TestLifecycleKindAndStageMatchTheCatalog(t *testing.T) {
	// Family 16, kind 11792, from the ref-30 block: kind = 4096 + 30*256 + stage.
	const wantStage = 16
	const wantEvent = 4096 + 30*256 + wantStage
	if StageLifecycle != wantStage {
		t.Errorf("StageLifecycle = %d, want %d", StageLifecycle, wantStage)
	}
	if EventLifecycle != wantEvent {
		t.Errorf("EventLifecycle = %d, want %d (4096 + 30*256 + %d)",
			EventLifecycle, wantEvent, wantStage)
	}
}
