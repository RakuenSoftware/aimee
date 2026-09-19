package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

const wikiPageLimit = 500
const wikiMaxBytes = 4 << 20

var errWikiTooLarge = errors.New("memory wiki exceeds the 4 MiB output limit")
var wikiPages = []struct{ kind, name, title string }{
	{"concept", "concepts.md", "Concepts"}, {"rule", "rules.md", "Rules"},
	{"preference", "preferences.md", "Preferences"}, {"episode", "episodes.md", "Episodes"},
	{"", "facts.md", "Facts"}, // The existing facts page includes every kind.
}

type wikiFile struct {
	Name             string `json:"name"`
	Text             string `json:"text"`
	PreserveExisting bool   `json:"preserve_existing,omitempty"`
}
type wikiBundle struct {
	Status string     `json:"status"`
	Files  []wikiFile `json:"files"`
}

func renderWikiPage(title string, records []publicMemoryRecord, remaining int) (string, error) {
	var out strings.Builder
	noun := "entries"
	if len(records) == 1 {
		noun = "entry"
	}
	fmt.Fprintf(&out, "# %s\n\n_%d %s_\n\n", title, len(records), noun)
	for _, r := range records {
		key := r.Key
		if key == "" {
			key = "(no key)"
		}
		// Fail the whole export rather than silently shortening a memory or page.
		if len(key)+len(r.Content)+len(r.ProvenanceCategory)+len(r.Kind)+len(r.Tier) > remaining-out.Len() {
			return "", errWikiTooLarge
		}
		fmt.Fprintf(&out, "## %s\n\n%s\n\n- tier: %s | kind: %s | confidence: %.2f", key, r.Content, r.Tier, r.Kind, r.Confidence)
		if r.ProvenanceCategory != "" {
			fmt.Fprintf(&out, " | provenance: %s", r.ProvenanceCategory)
		}
		out.WriteString("\n\n---\n\n")
		if out.Len() > remaining {
			return "", errWikiTooLarge
		}
	}
	return out.String(), nil
}

func buildWiki(now time.Time, load func(string) ([]publicMemoryRecord, error)) (json.RawMessage, error) {
	result := wikiBundle{Status: "ok", Files: make([]wikiFile, 0, 7)}
	counts := make([]int, len(wikiPages))
	remaining := wikiMaxBytes
	for i, page := range wikiPages {
		records, err := load(page.kind)
		if err != nil {
			return nil, err
		}
		text, err := renderWikiPage(page.title, records, remaining)
		if err != nil {
			return nil, err
		}
		remaining -= len(text)
		counts[i] = len(records)
		result.Files = append(result.Files, wikiFile{Name: page.name, Text: text})
	}
	result.Files = append(result.Files, wikiFile{Name: "log.md", PreserveExisting: true, Text: "# Memory State Log\n\n_Append-only log of memory state changes._\n\n_(No entries yet — populate via `aimee memory lint --fix` or future write hooks.)_\n"})
	var index strings.Builder
	fmt.Fprintf(&index, "# Memory Wiki\n\n_Generated: %s_\n\n## Pages\n\n", now.UTC().Format(time.RFC3339))
	for i, page := range wikiPages {
		fmt.Fprintf(&index, "- [%s](%s) — %d entries\n", page.title, page.name, counts[i])
	}
	index.WriteString("- [Log](log.md)\n")
	// Publish the table of contents last at the host filesystem boundary.
	result.Files = append(result.Files, wikiFile{Name: "index.md", Text: index.String()})
	encoded, err := json.Marshal(result)
	if err == nil && len(encoded) > wikiMaxBytes {
		return nil, errWikiTooLarge
	}
	return encoded, err
}

func (s *postgresDataStore) wiki(ctx context.Context, request DataRequest) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	request.Query, request.Tier, request.Limit = "", "", wikiPageLimit
	result, err := buildWiki(time.Now(), func(kind string) ([]publicMemoryRecord, error) {
		request.Kind = kind
		records, err := s.SearchVisible(ctx, request)
		if err != nil {
			return nil, err
		}
		return s.publicRecords(ctx, records)
	})
	if errors.Is(err, errWikiTooLarge) {
		return json.Marshal(commandError("capacity_exceeded", err.Error()))
	}
	return result, err
}
