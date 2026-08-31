package economizer

import (
	"fmt"
	"sort"
	"strings"
)

// Coordinate Closet — verbatim identifier conservation (fold §2).
//
// Folding replaces detail with a summary, and a summary paraphrases. An exact
// identifier that gets paraphrased is destroyed: a path becomes "the config
// file", a sha becomes "the commit". The closet is what makes folding safe to do
// at all — the exact bytes of every identifier in the folded region are carried
// forward verbatim, so the model can still act on them.
//
// Ported from the now-retired C Coordinate Closet. Determinism is a hard
// requirement, not a nicety: the folded prefix has to stay byte-identical across
// turns for the prompt cache to keep hitting, so ordering comes from an explicit
// total-order sort and never from map iteration.

// Lane is the provenance trust boundary.
type Lane int

const (
	// LaneAgent is agent/tool-originated content — trusted.
	LaneAgent Lane = 0
	// LaneUser is user-pasted content — quarantined, rendered below a divider
	// and marked untrusted so a conserved value can never be mistaken for an
	// instruction the agent produced.
	LaneUser Lane = 1
)

// CoordKind is the coordinate kind, which also drives the fallback label.
type CoordKind int

const (
	CoordKindUUID CoordKind = iota
	CoordKindSHA
	CoordKindPath
	CoordKindKV
	CoordKindRef
	CoordKindHandle
)

// Provenance records where a coordinate came from. Unknown numeric fields are -1.
type Provenance struct {
	Lane        Lane
	TurnID      int64
	ToolCallID  int64
	ResultIndex int
}

// DefaultProvenance is the AGENT lane with all ids unknown.
func DefaultProvenance() Provenance {
	return Provenance{Lane: LaneAgent, TurnID: -1, ToolCallID: -1, ResultIndex: -1}
}

// CoordEntry is one conserved coordinate.
type CoordEntry struct {
	Value       string // conserved verbatim
	Label       string // deterministic label
	Kind        CoordKind
	Prov        Provenance
	FirstOffset int // first-occurrence byte offset in the source
}

// ClosetConfig is the runtime config. Default-off.
type ClosetConfig struct {
	Enabled     bool
	BudgetBytes int    // hard byte cap for the rendered block; 0 = default
	MaxRatioPct int    // closet bytes <= rawLen * pct/100; 0 = default 100 (1x)
	Denylist    string // extra secret patterns (comma/space separated)
}

const (
	ClosetDefaultBudgetBytes = 2048
	ClosetDefaultMaxRatioPct = 100
)

// EvictResult reports whether everything nominated fit within the cap.
type EvictResult int

const (
	// EvictNone means all nominated coordinates were conserved.
	EvictNone EvictResult = 0
	// EvictFail means a nominated coordinate did not fit. Signalled, never a
	// silent drop: losing an identifier silently is exactly the failure the
	// closet exists to prevent.
	EvictFail EvictResult = 1
)

// ---------------------------------------------------------------- ASCII ctype
//
// ASCII-only throughout, never Unicode-aware helpers: byte-identical output must
// not depend on locale or on how a rune classifies.

func aLower(c byte) byte {
	if c >= 'A' && c <= 'Z' {
		return c + 32
	}
	return c
}

func aAlpha(c byte) bool { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') }
func aDigit(c byte) bool { return c >= '0' && c <= '9' }
func aAlnum(c byte) bool { return aAlpha(c) || aDigit(c) }
func isHex(c byte) bool {
	return aDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

// ciContains reports case-insensitive substring containment, ASCII-folded.
func ciContains(hay, needle string) bool {
	if hay == "" || needle == "" {
		return false
	}
	return strings.Contains(asciiLower(hay), asciiLower(needle))
}

func asciiLower(s string) string {
	b := []byte(s)
	for i := range b {
		b[i] = aLower(b[i])
	}
	return string(b)
}

// isIdentBoundary reports whether c may NOT be part of an identifier token, used
// for left-side word boundaries when deciding where a match may start.
func isIdentBoundary(c byte) bool {
	return !(aAlnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ':')
}

// tokenContinues reports whether a token continues into more identifier text.
// A hex run followed by alnum/_/- is part of a longer token and must not be
// conserved as a truncated prefix; ':' '.' '/' and space terminate it.
func tokenContinues(c byte) bool { return aAlnum(c) || c == '_' || c == '-' }

// ------------------------------------------------------------------ matchers
//
// Each returns the matched length at the start of s, or 0.

// matchUUID matches 8-4-4-4-12 hex with hyphens.
func matchUUID(s string) int {
	groups := [5]int{8, 4, 4, 4, 12}
	off := 0
	for g := 0; g < 5; g++ {
		for i := 0; i < groups[g]; i++ {
			if off >= len(s) || !isHex(s[off]) {
				return 0
			}
			off++
		}
		if g < 4 {
			if off >= len(s) || s[off] != '-' {
				return 0
			}
			off++
		}
	}
	// reject if this 36-char form is a prefix of a longer hex/identifier run
	if off < len(s) && tokenContinues(s[off]) {
		return 0
	}
	return off
}

// matchHandle matches handle:<id> / memory:<id>, returning the label too.
func matchHandle(s string) (int, string) {
	for _, prefix := range []string{"handle:", "memory:"} {
		if !strings.HasPrefix(s, prefix) {
			continue
		}
		off := len(prefix)
		for off < len(s) && (aAlnum(s[off]) || s[off] == '_' || s[off] == '-') {
			off++
		}
		if off > len(prefix) {
			return off, prefix[:len(prefix)-1] // drop ':'
		}
	}
	return 0, ""
}

// matchKV matches a digit-bearing key=value, returning the full matched length,
// the lowercased key as the label, and the value span within s.
func matchKV(s string) (matched int, label string, vstart, vend int) {
	off := 0
	if off >= len(s) || !(aAlpha(s[off]) || s[off] == '_') {
		return 0, "", 0, 0
	}
	kstart := off
	for off < len(s) && (aAlnum(s[off]) || s[off] == '_') {
		off++
	}
	kend := off
	if off >= len(s) || s[off] != '=' {
		return 0, "", 0, 0
	}
	off++ // '='
	vs := off
	hasDigit := false
	for off < len(s) && (aAlnum(s[off]) || s[off] == '.' || s[off] == '-' || s[off] == ':' || s[off] == '_') {
		if aDigit(s[off]) {
			hasDigit = true
		}
		off++
	}
	if off == vs || !hasDigit {
		return 0, "", 0, 0
	}
	ve := off
	for ve > vs && s[ve-1] == '.' { // trim trailing punctuation dot
		ve--
	}
	return off, asciiLower(s[kstart:kend]), vs, ve
}

// matchPath matches an absolute or repo-relative path.
//
// Anchored paths (/x, ./x, ../x) are unambiguous: one slash is enough. A BARE
// repo-relative path has no such marker and shares its shape with ordinary prose
// — "and/or", "he/she", "24/7", "2026/08/10" — so it needs a stricter test, or
// the closet fills with noise and evicts real coordinates to stay in budget.
//
// Bare form requires ALL of:
//   - at least one letter (rejects "2026/08/10", "24/7")
//   - a dot in the final segment, OR two or more slashes (rejects "and/or",
//     "he/she", "TODO/FIXME"; accepts "scripts/x.py" and "src/modules/git")
//
// The conservative casualty is a one-slash extensionless relative path
// ("src/server"), which stays unmatched rather than admitting every "and/or".
// This gap was measured, not guessed: with bare paths unmatched, the
// record-derived compaction summary retained 0/3 relative source paths that the
// legacy prose scan caught 3/3 (benchmarks/compaction-quality).
func matchPath(s string) int {
	if len(s) == 0 {
		return 0
	}
	anchored := s[0] == '/' ||
		(len(s) >= 2 && s[0] == '.' && s[1] == '/') ||
		(len(s) >= 3 && s[0] == '.' && s[1] == '.' && s[2] == '/')
	off, slashes, letters, lastSlash := 0, 0, 0, 0
	for off < len(s) && (aAlnum(s[off]) || s[off] == '/' || s[off] == '.' || s[off] == '_' || s[off] == '-') {
		if s[off] == '/' {
			lastSlash, slashes = off, slashes+1
		} else if aAlpha(s[off]) {
			letters++
		}
		off++
	}
	if slashes < 1 || off < 3 {
		return 0
	}
	for off > 0 && s[off-1] == '.' { // trim a trailing sentence dot
		off--
	}
	if !anchored {
		// Re-test the dot AFTER trimming: "src/foo." must not qualify on a dot
		// that was only sentence punctuation.
		dotInFinal := false
		for i := lastSlash + 1; i < off; i++ {
			if s[i] == '.' {
				dotInFinal = true
			}
		}
		if letters == 0 {
			return 0
		}
		if !dotInFinal && slashes < 2 {
			return 0
		}
	}
	return off
}

// matchSHA matches a 7..64 char hex run with at least one a-f letter, so plain
// decimal runs fall through to kv/ref. A longer run, or one continuing into
// other identifier text, is rejected rather than conserved as a truncated prefix.
func matchSHA(s string) int {
	off, alpha := 0, false
	for off < len(s) && off <= 64 && isHex(s[off]) {
		if !aDigit(s[off]) {
			alpha = true
		}
		off++
	}
	if off < 7 || off > 64 || !alpha {
		return 0
	}
	// reject only if the run continues into more identifier text; a sha may be
	// legitimately followed by ':' '.' '/' or whitespace
	if off < len(s) && tokenContinues(s[off]) {
		return 0
	}
	return off
}

// matchRef matches an issue/PR ref: '#' followed by 1+ digits.
func matchRef(s string) int {
	if len(s) < 2 || s[0] != '#' || !aDigit(s[1]) {
		return 0
	}
	off := 1
	for off < len(s) && aDigit(s[off]) {
		off++
	}
	return off
}

// CoordSet accumulates nominated coordinates, deduped by (lane, value) keeping
// the earliest occurrence.
type CoordSet struct {
	Items []CoordEntry
	seen  map[string]bool
}

func (set *CoordSet) has(value string, lane Lane) bool {
	if set.seen == nil {
		return false
	}
	return set.seen[fmt.Sprintf("%d\x00%s", lane, value)]
}

func (set *CoordSet) add(value, label string, kind CoordKind, prov Provenance, offset int) {
	if value == "" || set.has(value, prov.Lane) {
		return
	}
	if set.seen == nil {
		set.seen = map[string]bool{}
	}
	set.seen[fmt.Sprintf("%d\x00%s", prov.Lane, value)] = true
	set.Items = append(set.Items, CoordEntry{
		Value: value, Label: label, Kind: kind, Prov: prov, FirstOffset: offset,
	})
}

// NominateInto scans raw for verbatim coordinates, appending them to set, and
// returns the number of distinct coordinates added.
//
// A single left-to-right scan with a fixed matcher priority
// (uuid > handle > kv > path > sha > ref) so matches never overlap and the
// first-occurrence offset is well defined.
func NominateInto(raw string, prov *Provenance, set *CoordSet) int {
	if raw == "" || set == nil {
		return 0
	}
	pv := DefaultProvenance()
	if prov != nil {
		pv = *prov
	}
	before := len(set.Items)

	for i := 0; i < len(raw); {
		// Only attempt a match at an identifier boundary so we never start
		// mid-token (keeps offsets and dedup stable).
		if !(i == 0 || isIdentBoundary(raw[i-1])) {
			i++
			continue
		}
		p := raw[i:]

		if m := matchUUID(p); m > 0 {
			set.add(p[:m], "uuid", CoordKindUUID, pv, i)
			i += m
			continue
		}
		if m, lbl := matchHandle(p); m > 0 {
			set.add(p[:m], lbl, CoordKindHandle, pv, i)
			i += m
			continue
		}
		if m, lbl, vs, ve := matchKV(p); m > 0 {
			set.add(p[vs:ve], lbl, CoordKindKV, pv, i)
			i += m
			continue
		}
		if m := matchPath(p); m > 0 {
			set.add(p[:m], "path", CoordKindPath, pv, i)
			i += m
			continue
		}
		if m := matchSHA(p); m > 0 {
			set.add(p[:m], "sha", CoordKindSHA, pv, i)
			i += m
			continue
		}
		if m := matchRef(p); m > 0 {
			set.add(p[:m], "ref", CoordKindRef, pv, i)
			i += m
			continue
		}
		i++
	}
	return len(set.Items) - before
}

// Nominate is the convenience form used where the caller only wants the entries.
func Nominate(raw string, prov *Provenance) []CoordEntry {
	var set CoordSet
	NominateInto(raw, prov, &set)
	return set.Items
}

// ------------------------------------------------------------------ secrets

// labelIsSensitive reports whether a key name looks like a credential, so a
// value labelled that way is redacted even when the value itself has no
// recognizable prefix (e.g. aws_secret_access_key=...).
//
// Substring match, erring toward over-redaction rather than leaking: a
// non-secret value labelled like a credential is rare and safe to redact.
func labelIsSensitive(label string) bool {
	names := []string{
		"secret", "password", "passwd", "token",
		"apikey", "api_key", "api-key", "authorization",
		"credential", "private_key", "session_token",
	}
	if label == "" {
		return false
	}
	for _, n := range names {
		if ciContains(label, n) {
			return true
		}
	}
	return false
}

// IsSecret reports whether value matches a built-in secret pattern or any
// comma/space-separated substring in extraDenylist.
func IsSecret(value, extraDenylist string) bool {
	if value == "" {
		return false
	}
	// Token prefixes for common credential formats. Case-SENSITIVE: these are
	// exact issuer prefixes. sk- covers Anthropic's sk-ant- and OpenAI's sk-.
	prefixes := []string{
		"ghp_", "gho_", "ghs_", "ghu_", "github_pat_", "sk-", "xoxb-", "xoxp-", "xoxa-",
		"xapp-", "AKIA", "ASIA", "AIza", "ya29.", "npm_", "glpat-", "eyJ", "-----BEGIN",
	}
	for _, p := range prefixes {
		if strings.HasPrefix(value, p) {
			return true
		}
	}
	// Credential-bearing paths.
	for _, p := range []string{"id_rsa", "id_ed25519", "id_ecdsa", ".pem", "/.ssh/", ".env", "credentials"} {
		if ciContains(value, p) {
			return true
		}
	}
	// Caller-configured deny-list: comma/space separated substrings.
	for _, tok := range strings.FieldsFunc(extraDenylist, func(r rune) bool {
		return r == ',' || r == ' ' || r == '\t'
	}) {
		if ciContains(value, tok) {
			return true
		}
	}
	return false
}

// ------------------------------------------------------------------ render

// closetLine composes one entry's rendered line.
func closetLine(e CoordEntry, denylist string) string {
	suffix := ""
	if e.Prov.Lane == LaneUser {
		suffix = " (untrusted)"
	}
	if IsSecret(e.Value, denylist) || labelIsSensitive(e.Label) {
		return fmt.Sprintf("  [redacted:%s] ⟦%s⟧%s\n", e.Label, e.Label, suffix)
	}
	return fmt.Sprintf("  %s ⟦%s⟧%s\n", e.Value, e.Label, suffix)
}

const (
	closetHeader        = "Coordinate Closet (conserved from folded turns):\n"
	closetHeaderPartial = "Coordinate Closet (partial — some identifiers omitted, raise budget):\n"
	closetDivider       = "  -- user-supplied (untrusted) --\n"
	closetTruncNote     = "  [...additional identifiers omitted...]\n"
)

// RenderCloset renders the conserved block, returning the text and whether
// anything had to be dropped.
//
// Ordering is a TOTAL order — (lane, label, first offset), tie-broken by
// provenance ids then value — because a stable byte-for-byte rendering is what
// keeps the folded prefix cacheable.
//
// Bounded by min(budget, rawLen * maxRatioPct/100). If a nominated entry cannot
// be conserved within the cap the result is EvictFail; a dropped identifier is
// always announced, never silently lost.
func RenderCloset(set *CoordSet, cfg ClosetConfig, rawLen int) (string, EvictResult) {
	if set == nil || len(set.Items) == 0 || !cfg.Enabled {
		return "", EvictNone
	}

	budget := cfg.BudgetBytes
	if budget <= 0 {
		budget = ClosetDefaultBudgetBytes
	}
	ratio := cfg.MaxRatioPct
	if ratio <= 0 {
		ratio = ClosetDefaultMaxRatioPct
	}
	ratioCap := rawLen
	if ratio < 100 {
		ratioCap = rawLen * ratio / 100
	}
	cap := budget
	if ratioCap < cap {
		cap = ratioCap
	}

	sorted := append([]CoordEntry(nil), set.Items...)
	sort.SliceStable(sorted, func(a, b int) bool { return entryLess(sorted[a], sorted[b]) })

	// Pass 1: how many entries fit. Room for the truncation note is always
	// reserved so it can be appended if anything is dropped.
	total := len(closetHeaderPartial) // the longest header; budget against it
	fit := 0
	failed := false
	userDiv := false
	for i := range sorted {
		add := 0
		if sorted[i].Prov.Lane == LaneUser && !userDiv {
			add += len(closetDivider)
		}
		add += len(closetLine(sorted[i], cfg.Denylist))
		if total+add+len(closetTruncNote)+1 > cap {
			failed = true
			break
		}
		total += add
		fit++
		if sorted[i].Prov.Lane == LaneUser {
			userDiv = true
		}
	}

	if fit == 0 {
		return "", EvictFail
	}

	var b strings.Builder
	if failed {
		b.WriteString(closetHeaderPartial)
	} else {
		b.WriteString(closetHeader)
	}
	userDiv = false
	for i := 0; i < fit; i++ {
		if sorted[i].Prov.Lane == LaneUser && !userDiv {
			b.WriteString(closetDivider)
			userDiv = true
		}
		b.WriteString(closetLine(sorted[i], cfg.Denylist))
	}
	if failed {
		b.WriteString(closetTruncNote)
		return b.String(), EvictFail
	}
	return b.String(), EvictNone
}

// entryLess is the total order used for rendering.
func entryLess(x, y CoordEntry) bool {
	if x.Prov.Lane != y.Prov.Lane {
		return x.Prov.Lane < y.Prov.Lane
	}
	if x.Label != y.Label {
		return x.Label < y.Label
	}
	if x.FirstOffset != y.FirstOffset {
		return x.FirstOffset < y.FirstOffset
	}
	if x.Prov.TurnID != y.Prov.TurnID {
		return x.Prov.TurnID < y.Prov.TurnID
	}
	if x.Prov.ToolCallID != y.Prov.ToolCallID {
		return x.Prov.ToolCallID < y.Prov.ToolCallID
	}
	if x.Prov.ResultIndex != y.Prov.ResultIndex {
		return x.Prov.ResultIndex < y.Prov.ResultIndex
	}
	return x.Value < y.Value // total order
}
