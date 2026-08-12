package economizer

import (
	"sort"
	"unicode/utf16"
	"unicode/utf8"
)

// Strict, deterministic JSON whitespace compaction for authenticated local tool
// output.
//
// Ported from src/modules/economizer/economizer_json.c.
//
// The shape matters: VALIDATE COMPLETELY FIRST, then do a byte-copy pass that
// removes only RFC 8259 whitespace outside strings. Every non-whitespace source
// byte is retained in source order — this is not a re-serializer. That is
// deliberate: re-encoding would renormalize numbers and escapes, so the output
// would no longer be the caller's bytes, and a signature or digest over the
// original would stop matching.
//
// encoding/json cannot stand in for the validator: it accepts duplicate object
// names, which this rejects, and it would not enforce the depth or member caps.

// JSONResult enumerates the compaction outcomes.
type JSONResult int

const (
	JSONOK JSONResult = iota
	JSONInvalidArgument
	JSONTooLarge
	JSONTooDeep
	JSONInvalidUTF8
	JSONInvalidSyntax
	JSONDuplicateKey
	JSONNotShorter
)

// String renders the result the way econ_json_result_str does.
func (r JSONResult) String() string {
	switch r {
	case JSONOK:
		return "ok"
	case JSONInvalidArgument:
		return "invalid_argument"
	case JSONTooLarge:
		return "too_large"
	case JSONTooDeep:
		return "too_deep"
	case JSONInvalidUTF8:
		return "invalid_utf8"
	case JSONInvalidSyntax:
		return "invalid_syntax"
	case JSONDuplicateKey:
		return "duplicate_key"
	case JSONNotShorter:
		return "not_shorter"
	}
	return "unknown"
}

// Limits. Each is a refusal boundary, not a truncation point: exceeding one
// fails the whole compaction rather than emitting a partial document.
const (
	JSONMaxInput         = 16 * 1024 * 1024
	JSONMaxDepth         = 64
	JSONMaxObjectMembers = 65536
)

type jsonParser struct {
	s []byte
	p int
}

// jsonWS is RFC 8259 whitespace — exactly these four bytes, nothing else.
func jsonWS(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r'
}

func (p *jsonParser) skipWS() {
	for p.p < len(p.s) && jsonWS(p.s[p.p]) {
		p.p++
	}
}

func hexVal(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

func (p *jsonParser) hex4() (uint32, bool) {
	if len(p.s)-p.p < 4 {
		return 0, false
	}
	var v uint32
	for i := 0; i < 4; i++ {
		h := hexVal(p.s[p.p+i])
		if h < 0 {
			return 0, false
		}
		v = v<<4 | uint32(h)
	}
	p.p += 4
	return v, true
}

// utf8Sequence validates one UTF-8 sequence and returns its length.
//
// The ranges are spelled out rather than delegated, to keep the C's exact
// acceptance set: overlong forms, surrogates encoded as UTF-8 (ED A0..BF), and
// anything above U+10FFFF are all rejected.
func utf8Sequence(s []byte) (int, bool) {
	n := len(s)
	if n == 0 {
		return 0, false
	}
	a := s[0]
	cont := func(i int) bool { return n > i && s[i]&0xc0 == 0x80 }
	switch {
	case a < 0x80:
		return 1, true
	case a >= 0xc2 && a <= 0xdf && cont(1):
		return 2, true
	case a == 0xe0 && n >= 3 && s[1] >= 0xa0 && s[1] <= 0xbf && cont(2):
		return 3, true
	case a >= 0xe1 && a <= 0xec && cont(1) && cont(2):
		return 3, true
	case a == 0xed && n >= 3 && s[1] >= 0x80 && s[1] <= 0x9f && cont(2):
		return 3, true // ED A0.. would be a surrogate: excluded
	case a >= 0xee && a <= 0xef && cont(1) && cont(2):
		return 3, true
	case a == 0xf0 && n >= 4 && s[1] >= 0x90 && s[1] <= 0xbf && cont(2) && cont(3):
		return 4, true
	case a >= 0xf1 && a <= 0xf3 && cont(1) && cont(2) && cont(3):
		return 4, true
	case a == 0xf4 && n >= 4 && s[1] >= 0x80 && s[1] <= 0x8f && cont(2) && cont(3):
		return 4, true
	}
	return 0, false
}

// parseString validates a string and, when decode is set, returns its DECODED
// bytes — which is what duplicate-key detection must compare, since "a" and
// "a" are the same name.
func (p *jsonParser) parseString(decode bool) ([]byte, JSONResult) {
	if p.p >= len(p.s) || p.s[p.p] != '"' {
		return nil, JSONInvalidSyntax
	}
	p.p++
	var out []byte
	for p.p < len(p.s) {
		c := p.s[p.p]
		if c == '"' {
			p.p++
			return out, JSONOK
		}
		if c < 0x20 {
			return nil, JSONInvalidSyntax // raw control bytes are not permitted
		}
		if c == '\\' {
			p.p++
			if p.p >= len(p.s) {
				return nil, JSONInvalidSyntax
			}
			e := p.s[p.p]
			p.p++
			if e == 'u' {
				cp, ok := p.hex4()
				if !ok {
					return nil, JSONInvalidSyntax
				}
				if cp >= 0xd800 && cp <= 0xdbff {
					// A high surrogate MUST be followed by its low half.
					if len(p.s)-p.p < 6 || p.s[p.p] != '\\' || p.s[p.p+1] != 'u' {
						return nil, JSONInvalidSyntax
					}
					p.p += 2
					low, ok := p.hex4()
					if !ok || low < 0xdc00 || low > 0xdfff {
						return nil, JSONInvalidSyntax
					}
					cp = uint32(utf16.DecodeRune(rune(cp), rune(low)))
				} else if cp >= 0xdc00 && cp <= 0xdfff {
					return nil, JSONInvalidSyntax // a lone low surrogate
				}
				if decode {
					out = utf8.AppendRune(out, rune(cp))
				}
				continue
			}
			var v byte
			switch e {
			case '"':
				v = '"'
			case '\\':
				v = '\\'
			case '/':
				v = '/'
			case 'b':
				v = '\b'
			case 'f':
				v = '\f'
			case 'n':
				v = '\n'
			case 'r':
				v = '\r'
			case 't':
				v = '\t'
			default:
				return nil, JSONInvalidSyntax
			}
			if decode {
				out = append(out, v)
			}
			continue
		}
		// A raw byte: validate the UTF-8 sequence and copy it verbatim.
		sz, ok := utf8Sequence(p.s[p.p:])
		if !ok {
			return nil, JSONInvalidUTF8
		}
		if decode {
			out = append(out, p.s[p.p:p.p+sz]...)
		}
		p.p += sz
	}
	return nil, JSONInvalidSyntax // unterminated
}

func (p *jsonParser) parseNumber() JSONResult {
	i := p.p
	s := p.s
	if i < len(s) && s[i] == '-' {
		i++
	}
	if i >= len(s) {
		return JSONInvalidSyntax
	}
	switch {
	case s[i] == '0':
		i++ // a leading zero may not be followed by more digits
	case s[i] >= '1' && s[i] <= '9':
		for i < len(s) && s[i] >= '0' && s[i] <= '9' {
			i++
		}
	default:
		return JSONInvalidSyntax
	}
	if i < len(s) && s[i] == '.' {
		i++
		if i >= len(s) || s[i] < '0' || s[i] > '9' {
			return JSONInvalidSyntax
		}
		for i < len(s) && s[i] >= '0' && s[i] <= '9' {
			i++
		}
	}
	if i < len(s) && (s[i] == 'e' || s[i] == 'E') {
		i++
		if i < len(s) && (s[i] == '+' || s[i] == '-') {
			i++
		}
		if i >= len(s) || s[i] < '0' || s[i] > '9' {
			return JSONInvalidSyntax
		}
		for i < len(s) && s[i] >= '0' && s[i] <= '9' {
			i++
		}
	}
	p.p = i
	return JSONOK
}

func (p *jsonParser) parseArray(depth int) JSONResult {
	p.p++ // '['
	p.skipWS()
	if p.p < len(p.s) && p.s[p.p] == ']' {
		p.p++
		return JSONOK
	}
	for {
		if r := p.parseValue(depth + 1); r != JSONOK {
			return r
		}
		p.skipWS()
		if p.p >= len(p.s) {
			return JSONInvalidSyntax
		}
		switch p.s[p.p] {
		case ',':
			p.p++
			p.skipWS()
		case ']':
			p.p++
			return JSONOK
		default:
			return JSONInvalidSyntax
		}
	}
}

func (p *jsonParser) parseObject(depth int) JSONResult {
	p.p++ // '{'
	p.skipWS()
	if p.p < len(p.s) && p.s[p.p] == '}' {
		p.p++
		return JSONOK
	}
	var keys [][]byte
	for {
		p.skipWS()
		// Names are compared DECODED, so "a" and "a" collide as they should.
		key, r := p.parseString(true)
		if r != JSONOK {
			return r
		}
		if len(keys) >= JSONMaxObjectMembers {
			return JSONTooDeep
		}
		keys = append(keys, key)
		p.skipWS()
		if p.p >= len(p.s) || p.s[p.p] != ':' {
			return JSONInvalidSyntax
		}
		p.p++
		p.skipWS()
		if r := p.parseValue(depth + 1); r != JSONOK {
			return r
		}
		p.skipWS()
		if p.p >= len(p.s) {
			return JSONInvalidSyntax
		}
		switch p.s[p.p] {
		case ',':
			p.p++
		case '}':
			p.p++
			if !keysUnique(keys) {
				return JSONDuplicateKey
			}
			return JSONOK
		default:
			return JSONInvalidSyntax
		}
	}
}

// keysUnique reports whether every decoded name in the object is distinct.
func keysUnique(keys [][]byte) bool {
	if len(keys) < 2 {
		return true
	}
	sorted := make([][]byte, len(keys))
	copy(sorted, keys)
	sort.Slice(sorted, func(a, b int) bool { return string(sorted[a]) < string(sorted[b]) })
	for i := 1; i < len(sorted); i++ {
		if string(sorted[i]) == string(sorted[i-1]) {
			return false
		}
	}
	return true
}

func (p *jsonParser) parseValue(depth int) JSONResult {
	if depth >= JSONMaxDepth {
		return JSONTooDeep
	}
	p.skipWS()
	if p.p >= len(p.s) {
		return JSONInvalidSyntax
	}
	switch c := p.s[p.p]; {
	case c == '{':
		return p.parseObject(depth)
	case c == '[':
		return p.parseArray(depth)
	case c == '"':
		_, r := p.parseString(false)
		return r
	case c == 't':
		return p.lit("true")
	case c == 'f':
		return p.lit("false")
	case c == 'n':
		return p.lit("null")
	default:
		return p.parseNumber()
	}
}

func (p *jsonParser) lit(word string) JSONResult {
	if len(p.s)-p.p < len(word) || string(p.s[p.p:p.p+len(word)]) != word {
		return JSONInvalidSyntax
	}
	p.p += len(word)
	return JSONOK
}

// JSONCompact removes only RFC 8259 whitespace outside strings.
//
// Returns JSONNotShorter when the document contained no removable whitespace:
// there is no point shipping an identical copy, and reporting it distinctly lets
// the caller keep its original buffer.
func JSONCompact(input []byte) ([]byte, JSONResult) {
	if len(input) == 0 {
		return nil, JSONInvalidSyntax
	}
	if len(input) > JSONMaxInput {
		return nil, JSONTooLarge
	}

	// Validation is the boundary for the copy pass below: the complete document,
	// its UTF-8, escapes, depth and duplicate names are all checked BEFORE any
	// output is produced, so a partial or malformed document never yields bytes.
	p := &jsonParser{s: input}
	if r := p.parseValue(0); r != JSONOK {
		return nil, r
	}
	p.skipWS()
	if p.p != len(p.s) {
		return nil, JSONInvalidSyntax // trailing garbage
	}

	out := make([]byte, 0, len(input))
	inString, escaped := false, false
	for _, c := range input {
		if !inString && jsonWS(c) {
			continue
		}
		out = append(out, c)
		if inString {
			switch {
			case escaped:
				escaped = false
			case c == '\\':
				escaped = true
			case c == '"':
				inString = false
			}
		} else if c == '"' {
			inString = true
		}
	}
	if len(out) >= len(input) {
		return nil, JSONNotShorter
	}
	return out, JSONOK
}
