package economizer

import (
	"fmt"
	"strings"
)

// DefaultRecallTTLTurns is the anti-thrash residency: how many turns must pass
// before the same coordinate is surfaced again.
const DefaultRecallTTLTurns = 6

// RecallIndex is the §4 page table: coordinates that have LEFT the prompt in
// this conversation, so a later turn re-touching one can be told it is pageable
// rather than gone.
//
// This is what makes eviction REVERSIBLE, and reversible eviction is what
// licenses evicting continuously instead of waiting for a cliff. It must
// therefore outlive a single reduction, which is why it lives in the
// per-conversation state and not inside one fold call.
//
// Insertion order is preserved: rendering has to be deterministic for the
// prompt-cache to stay warm, so the map-iteration order Go would otherwise give
// us is not acceptable.
type RecallIndex struct {
	keys     []string
	lastTurn map[string]int
}

// NewRecallIndex returns an empty page table.
func NewRecallIndex() *RecallIndex {
	return &RecallIndex{lastTurn: map[string]int{}}
}

// Len reports how many coordinates are tracked.
func (ix *RecallIndex) Len() int {
	if ix == nil {
		return 0
	}
	return len(ix.keys)
}

// Keys returns the tracked coordinates in insertion order.
func (ix *RecallIndex) Keys() []string {
	if ix == nil {
		return nil
	}
	return append([]string(nil), ix.keys...)
}

// Add records one coordinate. Duplicates are ignored, and a re-added key keeps
// its existing residency so re-eviction cannot reset the anti-thrash clock.
func (ix *RecallIndex) Add(key string) {
	if ix == nil || key == "" {
		return
	}
	if ix.lastTurn == nil {
		ix.lastTurn = map[string]int{}
	}
	if _, seen := ix.lastTurn[key]; seen {
		return
	}
	ix.keys = append(ix.keys, key)
	ix.lastTurn[key] = -1
}

// LastTurn returns the turn a coordinate was last surfaced on, or -1 if never.
func (ix *RecallIndex) LastTurn(key string) int {
	if ix == nil || ix.lastTurn == nil {
		return -1
	}
	if t, ok := ix.lastTurn[key]; ok {
		return t
	}
	return -1
}

// SetLastTurn restores a coordinate's residency, so a page table carried across
// runs does not re-hint everything it already surfaced in the previous run.
func (ix *RecallIndex) SetLastTurn(key string, turn int) {
	if ix == nil || ix.lastTurn == nil {
		return
	}
	if _, ok := ix.lastTurn[key]; ok {
		ix.lastTurn[key] = turn
	}
}

// isCoordChar reports whether c can be part of a coordinate token.
func isCoordChar(c byte) bool {
	switch {
	case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9':
		return true
	}
	return c == '/' || c == '.' || c == '_' || c == '-' || c == ':'
}

// containsToken reports whole-token containment: key must appear in hay without
// being extended by a coordinate character on either side.
//
// Substring matching would hint constantly and wrongly — "/x" would fire on
// "/xyz", "memory:42" on "memory:420" — and a recall hint that fires on noise
// trains the model to ignore it.
func containsToken(hay, key string) bool {
	if key == "" {
		return false
	}
	for off := 0; ; {
		i := strings.Index(hay[off:], key)
		if i < 0 {
			return false
		}
		i += off
		beforeOK := i == 0 || !isCoordChar(hay[i-1])
		end := i + len(key)
		afterOK := end >= len(hay) || !isCoordChar(hay[end])
		if beforeOK && afterOK {
			return true
		}
		off = i + 1
	}
}

// AddFromText harvests coordinates out of evicted text and returns how many new
// ones were recorded.
//
// Only PATHs and HANDLEs are taken: the page table stores ADDRESSES a resolver
// can actually fetch. A conserved literal (a port number, a UUID) is a value,
// not an address — telling the model it can "page in" something unfetchable is
// worse than silence.
func (ix *RecallIndex) AddFromText(text string) int {
	if ix == nil || text == "" {
		return 0
	}
	before := ix.Len()
	// Provenance is irrelevant here: the page table stores addresses, not
	// conserved values, and never renders them into the prompt as trusted
	// content.
	for _, item := range Nominate(text, nil) {
		if item.Kind != CoordKindPath && item.Kind != CoordKindHandle {
			continue
		}
		ix.Add(item.Value)
	}
	return ix.Len() - before
}

// Detect surfaces coordinates that this turn re-touched, appending a hint line
// per coordinate to out, and returns how many were surfaced.
//
// A coordinate is skipped when it was surfaced fewer than ttlTurns ago: without
// that, a coordinate mentioned every turn would re-hint every turn.
func (ix *RecallIndex) Detect(turnText string, turn, ttlTurns int, out *strings.Builder) int {
	if ix == nil || turnText == "" {
		return 0
	}
	if ttlTurns <= 0 {
		ttlTurns = DefaultRecallTTLTurns
	}
	surfaced := 0
	for _, key := range ix.keys {
		if !containsToken(turnText, key) {
			continue // not re-touched this turn (whole-token match)
		}
		if last := ix.lastTurn[key]; last >= 0 && (turn-last) < ttlTurns {
			continue // surfaced too recently — anti-thrash
		}
		if out != nil {
			fmt.Fprintf(out, "  recall: %s was folded — page it back in via code_span_get/memory_get\n", key)
		}
		ix.lastTurn[key] = turn
		surfaced++
	}
	return surfaced
}
