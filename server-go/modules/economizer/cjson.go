package economizer

import (
	"encoding/json"
	"fmt"
	"math"
	"strconv"
	"strings"
)

// cJSON-compatible JSON printing.
//
// The fold embeds serialized sub-objects directly into its skeleton text (a
// tool_use input, an OpenAI tool_calls arguments string, a tool_result body) and
// hashes serialized messages for the freeze digest. Both of those live inside the
// folded prefix, which must stay byte-identical turn to turn or the prompt cache
// goes cold — the exact failure #2552 fixed.
//
// Go's encoding/json cannot be used for that text, because it differs from cJSON
// in two ways that change bytes without changing meaning:
//
//	cJSON_PrintUnformatted: {"zeta":1,"alpha":"a<b&c","path":"/x/y"}
//	Go json.Marshal(map):   {"alpha":"a<b&c","path":"/x/y","zeta":1}
//
//  1. Go sorts map keys; cJSON preserves insertion order.
//  2. Go HTML-escapes <, > and & by default.
//
// So this file reproduces cJSON's printer: insertion order preserved, cJSON's
// escape set, and cJSON's number spelling.

// JSONValue is an order-preserving parsed JSON value.
type JSONValue struct {
	Kind  JSONKind
	Str   string
	Num   float64
	Bool  bool
	Keys  []string     // object key order, as parsed
	Vals  []*JSONValue // object values, parallel to Keys
	Items []*JSONValue // array elements
}

// JSONKind enumerates the value kinds cJSON distinguishes.
type JSONKind int

const (
	JSONNull JSONKind = iota
	JSONFalse
	JSONTrue
	JSONNumber
	JSONString
	JSONArray
	JSONObject
)

// ParseJSON parses raw into an order-preserving tree, or returns nil if raw is
// not valid JSON (mirroring cJSON_ParseWithLength returning NULL).
func ParseJSON(raw string) *JSONValue {
	dec := json.NewDecoder(strings.NewReader(raw))
	dec.UseNumber()
	tok, err := dec.Token()
	if err != nil {
		return nil
	}
	v, err := parseJSONValue(dec, tok)
	if err != nil {
		return nil
	}
	// Trailing bytes after the first complete value are IGNORED, because
	// cJSON_ParseWithLength ignores them too (it is called without
	// require_null_terminated). Verified: cJSON parses `{"a":1}trailing` as
	// `{"a":1}`. Rejecting it here would make the fold take a different path than
	// C for the same input.
	return v
}

// KNOWN DIVERGENCE from cJSON, documented rather than hidden.
//
// cJSON accepts a RAW control byte (< 0x20) inside a string and re-emits it
// escaped: {"c":"x<0x01>y"} parses and prints as {"c":"xy"}. Go's
// encoding/json rejects that input outright, so ParseJSON returns nil where
// cJSON would have succeeded.
//
// Consequence: for a malformed-but-cJSON-acceptable body, the compress lever
// falls back to head+tail where C would have produced a JSON structural summary
// — different output bytes for the same input. Well-formed producers escape
// control characters, so this only reaches bodies that were already invalid
// JSON, but it IS a behaviour difference and is pinned by a test.
//
// Closing it needs a hand-written lenient parser; that is a deliberate
// follow-up, not an oversight.

func parseJSONValue(dec *json.Decoder, tok json.Token) (*JSONValue, error) {
	switch t := tok.(type) {
	case json.Delim:
		switch t {
		case '{':
			v := &JSONValue{Kind: JSONObject}
			for {
				kt, err := dec.Token()
				if err != nil {
					return nil, err
				}
				if d, ok := kt.(json.Delim); ok && d == '}' {
					return v, nil
				}
				key, _ := kt.(string)
				vt, err := dec.Token()
				if err != nil {
					return nil, err
				}
				child, err := parseJSONValue(dec, vt)
				if err != nil {
					return nil, err
				}
				v.Keys = append(v.Keys, key)
				v.Vals = append(v.Vals, child)
			}
		case '[':
			v := &JSONValue{Kind: JSONArray}
			for {
				vt, err := dec.Token()
				if err != nil {
					return nil, err
				}
				if d, ok := vt.(json.Delim); ok && d == ']' {
					return v, nil
				}
				child, err := parseJSONValue(dec, vt)
				if err != nil {
					return nil, err
				}
				v.Items = append(v.Items, child)
			}
		}
		return nil, fmt.Errorf("unexpected delimiter %v", t)
	case string:
		return &JSONValue{Kind: JSONString, Str: t}, nil
	case json.Number:
		f, err := t.Float64()
		if err != nil {
			return nil, err
		}
		return &JSONValue{Kind: JSONNumber, Num: f}, nil
	case bool:
		if t {
			return &JSONValue{Kind: JSONTrue, Bool: true}, nil
		}
		return &JSONValue{Kind: JSONFalse}, nil
	case nil:
		return &JSONValue{Kind: JSONNull}, nil
	}
	return nil, fmt.Errorf("unexpected token %v", tok)
}

// cjsonInt reproduces cJSON's valueint: the double truncated to int, clamped at
// the int bounds rather than wrapping.
func cjsonInt(d float64) int32 {
	if d >= math.MaxInt32 {
		return math.MaxInt32
	}
	if d <= math.MinInt32 {
		return math.MinInt32
	}
	return int32(d)
}

// compareDouble reproduces cJSON's compare_double: an EPSILON comparison, not
// exact equality.
//
// This is load-bearing and easy to miss. cJSON accepts the 15-digit spelling
// whenever it round-trips to within one DBL_EPSILON of the original, so
// 0.30000000000000004 prints as "0.3". An exact `parsed != d` check would fall
// through to 17 digits and emit "0.30000000000000004" instead — a different
// prefix byte for the same input, which is a cold cache.
func compareDouble(a, b float64) bool {
	maxVal := math.Abs(a)
	if math.Abs(b) > maxVal {
		maxVal = math.Abs(b)
	}
	// math.Nextafter(1,2)-1 is DBL_EPSILON (2^-52).
	const dblEpsilon = 2.220446049250313e-16
	return math.Abs(a-b) <= maxVal*dblEpsilon
}

// CJSONNumber spells a number the way cJSON's print_number does.
//
// Three branches, in cJSON's order: non-finite prints as null; a value equal to
// its clamped int prints as that int (so 3.0 is "3", not "3.0"); otherwise 15
// significant digits, falling back to 17 only when 15 does not round-trip to
// within DBL_EPSILON.
func CJSONNumber(d float64) string {
	if math.IsNaN(d) || math.IsInf(d, 0) {
		return "null"
	}
	if vi := cjsonInt(d); d == float64(vi) {
		return strconv.FormatInt(int64(vi), 10)
	}
	s := fmt.Sprintf("%.15g", d)
	if parsed, err := strconv.ParseFloat(s, 64); err != nil || !compareDouble(parsed, d) {
		s = fmt.Sprintf("%.17g", d)
	}
	return s
}

// cjsonEscape writes a JSON string literal using cJSON's escape set.
//
// cJSON escapes only ", \, and the control characters; it does NOT escape '/',
// and it does NOT escape '<', '>' or '&'. It also passes multi-byte UTF-8
// through untouched rather than emitting \uXXXX surrogate pairs.
func cjsonEscape(b *strings.Builder, s string) {
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			b.WriteString("\\\"")
		case '\\':
			b.WriteString("\\\\")
		case '\b':
			b.WriteString("\\b")
		case '\f':
			b.WriteString("\\f")
		case '\n':
			b.WriteString("\\n")
		case '\r':
			b.WriteString("\\r")
		case '\t':
			b.WriteString("\\t")
		default:
			if c < 32 {
				fmt.Fprintf(b, "\\u%04x", c)
			} else {
				b.WriteByte(c)
			}
		}
	}
	b.WriteByte('"')
}

// PrintJSONUnformatted renders v the way cJSON_PrintUnformatted does: no
// whitespace, object keys in their original order.
func PrintJSONUnformatted(v *JSONValue) string {
	var b strings.Builder
	printJSON(&b, v)
	return b.String()
}

func printJSON(b *strings.Builder, v *JSONValue) {
	if v == nil {
		b.WriteString("null")
		return
	}
	switch v.Kind {
	case JSONNull:
		b.WriteString("null")
	case JSONFalse:
		b.WriteString("false")
	case JSONTrue:
		b.WriteString("true")
	case JSONNumber:
		b.WriteString(CJSONNumber(v.Num))
	case JSONString:
		cjsonEscape(b, v.Str)
	case JSONArray:
		b.WriteByte('[')
		for i, item := range v.Items {
			if i > 0 {
				b.WriteByte(',')
			}
			printJSON(b, item)
		}
		b.WriteByte(']')
	case JSONObject:
		b.WriteByte('{')
		for i, key := range v.Keys {
			if i > 0 {
				b.WriteByte(',')
			}
			cjsonEscape(b, key)
			b.WriteByte(':')
			printJSON(b, v.Vals[i])
		}
		b.WriteByte('}')
	}
}
