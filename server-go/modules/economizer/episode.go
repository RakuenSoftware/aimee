package economizer

import "fmt"

// Sealed work episode — file inventory plus conclusion (fold §5).
//
// Ported from src/modules/economizer/episode_seal.c.
//
// Unlike a narrative episode card, a SEALED episode is a replayable checkpoint:
// the set of files touched plus what was concluded, so a later session that
// re-touches a member file can auto-recall what was learned rather than
// rediscovering it. That is the same reversibility idea as the §4 page table,
// applied across sessions instead of within one.

// EpisodeSealUnitType is the distinct memory_units.unit_type value for sealed
// episodes, kept separate from the narrative "episode_card" so the two are not
// overloaded onto one kind.
const EpisodeSealUnitType = "episode_seal"

// EpisodeSeal is a conclusion plus the file inventory it was drawn from.
//
// File order is insertion order, not sorted: serialization has to be
// deterministic, and preserving the order the agent touched things also keeps
// the record readable as a narrative of the work.
type EpisodeSeal struct {
	Conclusion string
	Files      []string
}

// SetConclusion replaces the conclusion text.
func (s *EpisodeSeal) SetConclusion(text string) {
	if s == nil {
		return
	}
	s.Conclusion = text
}

// AddFile adds a path to the inventory, deduplicated by exact match. An empty
// path is ignored rather than treated as an error — a turn that touched nothing
// is a normal turn, not a failure.
func (s *EpisodeSeal) AddFile(path string) {
	if s == nil || path == "" {
		return
	}
	for _, existing := range s.Files {
		if existing == path {
			return
		}
	}
	s.Files = append(s.Files, path)
}

// Touches is the auto-recall predicate: does this seal cover the given file?
//
// EXACT match, deliberately. A prefix or substring rule would fire on unrelated
// files that merely share a directory, and a recall that fires on noise is one
// the agent learns to ignore — the same reasoning as the page table's
// whole-token matching.
func (s *EpisodeSeal) Touches(path string) bool {
	if s == nil || path == "" {
		return false
	}
	for _, f := range s.Files {
		if f == path {
			return true
		}
	}
	return false
}

// Serialize renders the seal as deterministic JSON.
func (s *EpisodeSeal) Serialize() (string, error) {
	if s == nil {
		return "", fmt.Errorf("economizer: nil seal")
	}
	root := NewObject()
	root.Set("conclusion", NewString(s.Conclusion))
	files := NewArray()
	for _, f := range s.Files {
		files.Append(NewString(f))
	}
	root.Set("files", files)
	return PrintJSONUnformatted(root), nil
}

// ParseEpisodeSeal restores a seal from JSON.
//
// ALL-OR-NOTHING: it validates and builds into a temporary, replacing *s only on
// complete success, so malformed or partial JSON never destroys the caller's
// existing seal. A seal is a checkpoint; half a checkpoint is worse than none.
func ParseEpisodeSeal(s *EpisodeSeal, blob string) error {
	if s == nil || blob == "" {
		return fmt.Errorf("economizer: nothing to parse")
	}
	root := ParseJSON(blob)
	if root == nil || root.Kind != JSONObject {
		return fmt.Errorf("economizer: seal is not a JSON object")
	}
	files := root.Get("files")
	if files != nil && !files.IsArray() {
		return fmt.Errorf("economizer: seal files is not an array")
	}

	var tmp EpisodeSeal
	tmp.SetConclusion(root.GetString("conclusion"))
	if files != nil {
		for _, f := range files.Items {
			// A non-string entry is SKIPPED rather than failing the parse: one bad
			// element should not cost the whole checkpoint.
			if f != nil && f.Kind == JSONString {
				tmp.AddFile(f.Str)
			}
		}
	}

	*s = tmp
	return nil
}
