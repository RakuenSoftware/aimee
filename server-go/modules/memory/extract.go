package memory

import "strings"

// Pattern-first fact extraction and the retraction scan, ported from
// src/modules/memory/memory_extract_patterns.c.
//
// A high-precision pass that runs BEFORE the model on ingress text: structured
// value classifiers (IPv4/IPv6/MAC/email/ISO date) and one unambiguous sentence
// template emit candidate triples directly, so only the residual text needs the
// costly model rewrite. Precision buys cost, not a bypass of validation — every
// triple it emits still goes through GateCheck.
//
// Pure by construction, like the gate: no database, no config. That is what
// lets it run in the module process.

// ValueKind is a structured value shape. Values match pattern_value_kind_t.
type ValueKind uint32

const (
	ValueNone ValueKind = 0
	ValueIPv4 ValueKind = 1
	ValueIPv6 ValueKind = 2
	ValueMAC  ValueKind = 3
	// ValueEmail and ValueDate are the remaining scalar shapes.
	ValueEmail ValueKind = 4
	ValueDate  ValueKind = 5
)

// The bounds the production caller (db2_fact_ingest_text / db2_typed_fact_ingress)
// gives the C, reproduced because they are visible in the output: an attribute or
// value longer than its buffer comes back truncated, and a truncated attribute
// normalizes to a different relation name. Whatever a module returns has to match
// what core would have produced for the same turn.
const (
	attrMax  = 128
	valueMax = 128
)

// Triple is a candidate fact found before the model. RelType is a normalized
// guess; the gate still decides whether it is written and how.
type Triple struct {
	Subject     string
	RelType     string
	Object      string
	SubjectKind NodeKind
	ObjectKind  NodeKind
}

func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

func isHex(c byte) bool {
	return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func allDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		if !isDigit(s[i]) {
			return false
		}
	}
	return len(s) > 0
}

// isIPv4 recognizes a dotted quad: four 1-3 digit octets, each 0-255.
//
// A trailing dot is accepted ("10.0.0.1." is IPv4), because the octet loop
// consumes the separator and then finds the string ended. That is the C's
// behaviour and it matters here: the extractor hands values in with sentence
// punctuation still attached.
func isIPv4(t string) bool {
	octets, i := 0, 0
	for i < len(t) {
		n, val := 0, 0
		for i+n < len(t) && isDigit(t[i+n]) {
			val = val*10 + int(t[i+n]-'0')
			n++
			if n > 3 {
				return false
			}
		}
		if n == 0 || val > 255 {
			return false
		}
		i += n
		octets++
		if i < len(t) && t[i] == '.' {
			i++
		} else if i < len(t) {
			return false
		}
	}
	return octets == 4
}

// isMAC recognizes exactly six two-hex-digit groups joined by a single
// consistent ':' or '-'.
func isMAC(t string) bool {
	if len(t) != 17 {
		return false
	}
	sep := t[2]
	if sep != ':' && sep != '-' {
		return false
	}
	for g := 0; g < 6; g++ {
		group := t[g*3:]
		if !isHex(group[0]) || !isHex(group[1]) {
			return false
		}
		if g < 5 && group[2] != sep {
			return false
		}
	}
	return true
}

// isIPv6 is deliberately conservative: a "::" run, or exactly eight groups of
// 1-4 hex digits joined by ':'. The six-group form is a MAC, and ClassifyValue
// checks MAC first, so the shapes do not collide.
func isIPv6(t string) bool {
	if !strings.Contains(t, ":") {
		return false
	}
	for i := 0; i < len(t); i++ {
		if !isHex(t[i]) && t[i] != ':' {
			return false
		}
	}
	if strings.Contains(t, "::") {
		return true
	}
	groups, length := 0, 0
	for i := 0; i <= len(t); i++ {
		if i == len(t) || t[i] == ':' {
			if length < 1 || length > 4 {
				return false
			}
			groups++
			length = 0
			continue
		}
		length++
	}
	return groups == 8
}

// isEmail requires one '@', a non-empty local part, a domain with an interior
// dot, and conservative characters throughout.
func isEmail(t string) bool {
	at := strings.IndexByte(t, '@')
	if at <= 0 || strings.IndexByte(t[at+1:], '@') >= 0 {
		return false
	}
	domain := t[at+1:]
	dot := strings.IndexByte(domain, '.')
	if dot <= 0 || dot == len(domain)-1 {
		return false
	}
	for i := 0; i < len(t); i++ {
		c := t[i]
		if c == '@' || c == '.' || c == '-' || c == '_' || c == '+' || isAlnum(c) {
			continue
		}
		return false
	}
	return true
}

// isISODate recognizes YYYY-MM-DD with an in-range month and day. The day bound
// is the calendar-independent 31, not the month's own length, matching the C.
func isISODate(t string) bool {
	if len(t) != 10 || t[4] != '-' || t[7] != '-' {
		return false
	}
	if !allDigits(t[0:4]) || !allDigits(t[5:7]) || !allDigits(t[8:10]) {
		return false
	}
	month := int(t[5]-'0')*10 + int(t[6]-'0')
	day := int(t[8]-'0')*10 + int(t[9]-'0')
	return month >= 1 && month <= 12 && day >= 1 && day <= 31
}

// ClassifyValue classifies a whole token as a structured value shape. Whole-token
// match only — no substring — so precision stays high.
//
// The test order is the C's. It does not actually decide anything: the five
// shapes are mutually exclusive, so no token can reach two of them —
// TestValueShapesAreDisjoint holds that. (The C carries a comment saying MAC is
// tested first because a six-group colon form would otherwise read as IPv6, but
// its own isIPv6 requires eight groups or a "::" run, so the two never meet.)
func ClassifyValue(token string) ValueKind {
	switch {
	case token == "":
		return ValueNone
	case isIPv4(token):
		return ValueIPv4
	case isMAC(token):
		return ValueMAC
	case isIPv6(token):
		return ValueIPv6
	case isEmail(token):
		return ValueEmail
	case isISODate(token):
		return ValueDate
	}
	return ValueNone
}

// ValueNodeKind maps a value shape to the entity kind it implies.
func ValueNodeKind(kind ValueKind) NodeKind {
	switch kind {
	case ValueIPv4, ValueIPv6:
		return NodeIp
	case ValueMAC, ValueEmail, ValueDate:
		return NodeScalar
	}
	return NodeOther
}

// ciFind is a case-insensitive substring search from `from`, returning the index
// or -1.
func ciFind(haystack, needle string, from int) int {
	if from < 0 || from > len(haystack) {
		return -1
	}
	if needle == "" {
		return from
	}
	for i := from; i+len(needle) <= len(haystack); i++ {
		if ciStartsAt(haystack, i, needle) {
			return i
		}
	}
	return -1
}

// ciStartsAt reports whether needle sits at haystack[pos], case-insensitively.
// A haystack that ends first is a mismatch, mirroring the C's compare against
// the terminator.
func ciStartsAt(haystack string, pos int, needle string) bool {
	for k := 0; k < len(needle); k++ {
		if pos+k >= len(haystack) || toLower(haystack[pos+k]) != toLower(needle[k]) {
			return false
		}
	}
	return true
}

// retractionCues are kept specific on purpose. This is a recall-oriented
// pre-filter: a hit only flags the turn for closer inspection — the retraction
// itself still needs an explicit subject and relation — so it never deletes on
// its own. A bare "forget" would fire on "don't forget to call mom".
var retractionCues = []string{
	"forget that", "forget about", "forget my", "forget what",
	"delete that", "delete the", "that's wrong", "thats wrong",
	"that is wrong", "no longer", "scratch that", "ignore that",
	"never mind", "nevermind", "disregard",
}

// IsRetraction reports whether the text carries a retraction cue.
func IsRetraction(text string) bool {
	if text == "" {
		return false
	}
	for _, cue := range retractionCues {
		if ciFind(text, cue, 0) >= 0 {
			return true
		}
	}
	return false
}

// trimmedTo trims ASCII whitespace from both ends and then keeps at most cap-1
// bytes, in that order — the C trims into a fixed buffer, so truncation applies
// to the trimmed text, not the raw span.
func trimmedTo(s string, cap int) string {
	if cap <= 0 {
		return ""
	}
	start, end := 0, len(s)
	for start < end && isSpace(s[start]) {
		start++
	}
	for end > start && isSpace(s[end-1]) {
		end--
	}
	s = s[start:end]
	if len(s) > cap-1 {
		s = s[:cap-1]
	}
	return s
}

// isMyWord reports whether a "my" word starts at index i: a word boundary before
// it and whitespace after, so "army" does not match.
func isMyWord(text string, i int) bool {
	if i+1 >= len(text) || toLower(text[i]) != 'm' || toLower(text[i+1]) != 'y' {
		return false
	}
	if i > 0 && (isAlnum(text[i-1]) || text[i-1] == '_') {
		return false
	}
	return i+2 < len(text) && isSpace(text[i+2])
}

// PossessiveAttr returns the attribute a "my <attr>" possessive names, and
// whether one was present. The attribute stops at a sentence terminator, at the
// " is "/" was " that opens a value clause, or after about three words, so a
// retraction turn does not swallow the rest of the sentence.
//
// It pairs with IsRetraction to drive the retraction path. Because retraction
// only affects facts that already exist, an imprecise attribute safely no-ops.
func PossessiveAttr(text string) (string, bool) {
	for i := 0; i < len(text); i++ {
		if !isMyWord(text, i) {
			continue
		}
		start := i + 2
		for start < len(text) && isSpace(text[start]) {
			start++
		}
		end, words, inWord := start, 0, false
		for end < len(text) {
			c := text[end]
			if c == '.' || c == '!' || c == '?' || c == '\n' {
				break
			}
			if c == ' ' {
				if ciStartsAt(text, end, " is ") || ciStartsAt(text, end, " was ") {
					break
				}
				if inWord {
					words++
					if words >= 3 {
						break
					}
					inWord = false
				}
			} else {
				inWord = true
			}
			end++
		}
		attr := trimmedTo(text[start:end], attrMax)
		return attr, attr != ""
	}
	return "", false
}

// ExtractPatterns pulls candidate triples out of text, at most max of them.
//
// It recognizes one template — "my <attr> is <value>" -> (user, <attr>, <value>)
// — with the object kind inferred from the value's shape. Conservative by
// design: text it does not match yields nothing and is left for the model.
// A max of zero or less is the C's bad-arg case and returns nil.
func ExtractPatterns(text string, max int) []Triple {
	if max <= 0 {
		return nil
	}
	var triples []Triple
	for i := 0; i < len(text) && len(triples) < max; i++ {
		if !isMyWord(text, i) {
			continue
		}
		attrStart := i + 2
		for attrStart < len(text) && isSpace(text[attrStart]) {
			attrStart++
		}
		isPos := ciFind(text, " is ", attrStart)
		if isPos < 0 {
			continue
		}
		valStart := isPos + 4

		// The value runs to a sentence terminator or the end of the text. A '.',
		// '!' or '?' ends the sentence only when whitespace or the end follows, so
		// an interior dot ("example.com", "192.168.1.254") does not truncate it.
		valEnd := valStart
		for valEnd < len(text) {
			c := text[valEnd]
			if c == '\n' {
				break
			}
			if c == '.' || c == '!' || c == '?' {
				if valEnd+1 == len(text) || isSpace(text[valEnd+1]) {
					break
				}
			}
			valEnd++
		}

		attr := trimmedTo(text[attrStart:isPos], attrMax)
		value := trimmedTo(text[valStart:valEnd], valueMax)
		i = valEnd // continue past this match, whether or not it yielded a triple
		if attr == "" || value == "" {
			continue
		}

		relType := normalizeRelType(attr)
		if relType == "" {
			continue
		}
		triples = append(triples, Triple{
			Subject:     "user",
			RelType:     relType,
			Object:      value,
			SubjectKind: NodePerson,
			ObjectKind:  ValueNodeKind(ClassifyValue(value)),
		})
	}
	return triples
}
