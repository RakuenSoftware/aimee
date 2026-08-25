package delegates

import (
	"math"
	"strconv"
	"strings"
)

// A minimal JSON reader/writer that matches the C side byte for byte.
//
// encoding/json cannot be used here. Decoding into a map loses key order, and
// re-printing would reorder an arguments object that the C parser preserves --
// the rescued call would differ from the native one for no reason a reader
// could see. json.Marshal also escapes <, > and & where cJSON does not.
//
// Parsing deliberately accepts trailing text after a complete value, because
// cJSON_Parse does: it reads one value and stops. That is load-bearing, not an
// oversight -- it is why a fenced or prose-wrapped object is still recovered.

type jsonKind byte

const (
	jsonObject jsonKind = iota
	jsonArray
	jsonString
	jsonNumber
	jsonBool
	jsonNull
)

type jsonValue struct {
	kind    jsonKind
	str     string
	num     float64
	boolean bool
	keys    []string
	vals    []*jsonValue
	items   []*jsonValue
}

func (v *jsonValue) isObject() bool { return v != nil && v.kind == jsonObject }
func (v *jsonValue) isString() bool { return v != nil && v.kind == jsonString }

// get looks up a key case-sensitively, mirroring
// cJSON_GetObjectItemCaseSensitive.
func (v *jsonValue) get(key string) *jsonValue {
	if !v.isObject() {
		return nil
	}
	for i, k := range v.keys {
		if k == key {
			return v.vals[i]
		}
	}
	return nil
}

type jsonParser struct {
	s   string
	pos int
}

func (p *jsonParser) skipSpace() {
	for p.pos < len(p.s) && p.s[p.pos] <= ' ' && p.s[p.pos] != 0 {
		p.pos++
	}
}

// parseJSONPrefix reads one JSON value from the front of s, ignoring anything
// after it.
func parseJSONPrefix(s string) (*jsonValue, bool) {
	p := &jsonParser{s: s}
	p.skipSpace()
	v, ok := p.parseValue()
	if !ok {
		return nil, false
	}
	return v, true
}

func (p *jsonParser) parseValue() (*jsonValue, bool) {
	p.skipSpace()
	if p.pos >= len(p.s) {
		return nil, false
	}
	switch c := p.s[p.pos]; {
	case c == '{':
		return p.parseObject()
	case c == '[':
		return p.parseArray()
	case c == '"':
		str, ok := p.parseString()
		if !ok {
			return nil, false
		}
		return &jsonValue{kind: jsonString, str: str}, true
	case strings.HasPrefix(p.s[p.pos:], "true"):
		p.pos += 4
		return &jsonValue{kind: jsonBool, boolean: true}, true
	case strings.HasPrefix(p.s[p.pos:], "false"):
		p.pos += 5
		return &jsonValue{kind: jsonBool}, true
	case strings.HasPrefix(p.s[p.pos:], "null"):
		p.pos += 4
		return &jsonValue{kind: jsonNull}, true
	case c == '-' || (c >= '0' && c <= '9'):
		return p.parseNumber()
	}
	return nil, false
}

func (p *jsonParser) parseObject() (*jsonValue, bool) {
	p.pos++ // '{'
	obj := &jsonValue{kind: jsonObject}
	p.skipSpace()
	if p.pos < len(p.s) && p.s[p.pos] == '}' {
		p.pos++
		return obj, true
	}
	for {
		p.skipSpace()
		if p.pos >= len(p.s) || p.s[p.pos] != '"' {
			return nil, false
		}
		key, ok := p.parseString()
		if !ok {
			return nil, false
		}
		p.skipSpace()
		if p.pos >= len(p.s) || p.s[p.pos] != ':' {
			return nil, false
		}
		p.pos++
		val, ok := p.parseValue()
		if !ok {
			return nil, false
		}
		obj.keys = append(obj.keys, key)
		obj.vals = append(obj.vals, val)
		p.skipSpace()
		if p.pos >= len(p.s) {
			return nil, false
		}
		if p.s[p.pos] == ',' {
			p.pos++
			continue
		}
		if p.s[p.pos] == '}' {
			p.pos++
			return obj, true
		}
		return nil, false
	}
}

func (p *jsonParser) parseArray() (*jsonValue, bool) {
	p.pos++ // '['
	arr := &jsonValue{kind: jsonArray}
	p.skipSpace()
	if p.pos < len(p.s) && p.s[p.pos] == ']' {
		p.pos++
		return arr, true
	}
	for {
		val, ok := p.parseValue()
		if !ok {
			return nil, false
		}
		arr.items = append(arr.items, val)
		p.skipSpace()
		if p.pos >= len(p.s) {
			return nil, false
		}
		if p.s[p.pos] == ',' {
			p.pos++
			continue
		}
		if p.s[p.pos] == ']' {
			p.pos++
			return arr, true
		}
		return nil, false
	}
}

func (p *jsonParser) parseString() (string, bool) {
	if p.pos >= len(p.s) || p.s[p.pos] != '"' {
		return "", false
	}
	p.pos++
	var b strings.Builder
	for p.pos < len(p.s) {
		c := p.s[p.pos]
		if c == '"' {
			p.pos++
			return b.String(), true
		}
		if c != '\\' {
			b.WriteByte(c)
			p.pos++
			continue
		}
		p.pos++
		if p.pos >= len(p.s) {
			return "", false
		}
		switch p.s[p.pos] {
		case '"':
			b.WriteByte('"')
		case '\\':
			b.WriteByte('\\')
		case '/':
			b.WriteByte('/')
		case 'b':
			b.WriteByte('\b')
		case 'f':
			b.WriteByte('\f')
		case 'n':
			b.WriteByte('\n')
		case 'r':
			b.WriteByte('\r')
		case 't':
			b.WriteByte('\t')
		case 'u':
			if p.pos+4 >= len(p.s) {
				return "", false
			}
			code, err := strconv.ParseUint(p.s[p.pos+1:p.pos+5], 16, 32)
			if err != nil {
				return "", false
			}
			b.WriteRune(rune(code))
			p.pos += 4
		default:
			return "", false
		}
		p.pos++
	}
	return "", false
}

func (p *jsonParser) parseNumber() (*jsonValue, bool) {
	start := p.pos
	if p.pos < len(p.s) && (p.s[p.pos] == '-' || p.s[p.pos] == '+') {
		p.pos++
	}
	for p.pos < len(p.s) {
		c := p.s[p.pos]
		if (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-' {
			p.pos++
			continue
		}
		break
	}
	f, err := strconv.ParseFloat(p.s[start:p.pos], 64)
	if err != nil {
		return nil, false
	}
	return &jsonValue{kind: jsonNumber, num: f}, true
}

// printJSON renders compactly, exactly as cJSON_PrintUnformatted does.
func printJSON(v *jsonValue) string {
	var b strings.Builder
	writeJSON(&b, v)
	return b.String()
}

func writeJSON(b *strings.Builder, v *jsonValue) {
	if v == nil {
		b.WriteString("null")
		return
	}
	switch v.kind {
	case jsonNull:
		b.WriteString("null")
	case jsonBool:
		if v.boolean {
			b.WriteString("true")
		} else {
			b.WriteString("false")
		}
	case jsonNumber:
		b.WriteString(formatCJSONNumber(v.num))
	case jsonString:
		writeJSONString(b, v.str)
	case jsonArray:
		b.WriteByte('[')
		for i, item := range v.items {
			if i > 0 {
				b.WriteByte(',')
			}
			writeJSON(b, item)
		}
		b.WriteByte(']')
	case jsonObject:
		b.WriteByte('{')
		for i, k := range v.keys {
			if i > 0 {
				b.WriteByte(',')
			}
			writeJSONString(b, k)
			b.WriteByte(':')
			writeJSON(b, v.vals[i])
		}
		b.WriteByte('}')
	}
}

// formatCJSONNumber mirrors cJSON's print_number: an integral value that fits
// the int field prints as an integer, otherwise 15 significant digits, widened
// to 17 only when 15 does not round-trip.
func formatCJSONNumber(d float64) string {
	if math.IsNaN(d) || math.IsInf(d, 0) {
		return "null"
	}
	valueint := clampToInt(d)
	if d == float64(valueint) {
		return strconv.Itoa(valueint)
	}
	s := strconv.FormatFloat(d, 'g', 15, 64)
	if back, err := strconv.ParseFloat(s, 64); err != nil || back != d {
		s = strconv.FormatFloat(d, 'g', 17, 64)
	}
	return s
}

// clampToInt mirrors cJSON's parse-time saturation of valueint.
func clampToInt(d float64) int {
	const intMax = 2147483647
	const intMin = -2147483648
	switch {
	case d >= intMax:
		return intMax
	case d <= intMin:
		return intMin
	}
	return int(d)
}

// writeJSONString escapes exactly what cJSON escapes -- notably NOT <, > or &,
// which encoding/json would turn into < and friends.
func writeJSONString(b *strings.Builder, s string) {
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\b':
			b.WriteString(`\b`)
		case '\f':
			b.WriteString(`\f`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		case '\t':
			b.WriteString(`\t`)
		default:
			if c < 0x20 {
				const hex = "0123456789abcdef"
				b.WriteString(`\u00`)
				b.WriteByte(hex[c>>4])
				b.WriteByte(hex[c&0xf])
			} else {
				b.WriteByte(c)
			}
		}
	}
	b.WriteByte('"')
}

// orderedObject builds an arguments object in insertion order.
type orderedObject struct {
	keys []string
	vals []*jsonValue
}

func (o *orderedObject) addParameterValue(key, value string) {
	if key == "" {
		return
	}
	// A parameter that is itself JSON keeps its type; anything else is a
	// string. A model writing command=ls must not have "ls" parsed away.
	if parsed, ok := parseJSONPrefix(value); ok {
		o.keys = append(o.keys, key)
		o.vals = append(o.vals, parsed)
		return
	}
	o.keys = append(o.keys, key)
	o.vals = append(o.vals, &jsonValue{kind: jsonString, str: value})
}

func (o *orderedObject) print() string {
	return printJSON(&jsonValue{kind: jsonObject, keys: o.keys, vals: o.vals})
}
