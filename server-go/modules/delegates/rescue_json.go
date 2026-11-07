package delegates

import (
	"encoding/json"
	"github.com/JBailes/aimee/server-go/ir"
	"math"
	"strconv"
	"strings"
)

// Ordered native-entry JSON values retain exact decimals. Keys and values never
// pass through a map/double roundtrip before canonical tool-call admission.

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
	kind         jsonKind
	str          string
	num          float64
	numberLexeme string
	boolean      bool
	keys         []string
	vals         []*jsonValue
	items        []*jsonValue
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

// Prose scanning may select a complete leading JSON value. The selected value
// itself uses the strict shared lexical contract before any dialect lookup.
func parseJSONPrefix(source string) (*jsonValue, bool) {
	decoder := json.NewDecoder(strings.NewReader(source))
	var raw json.RawMessage
	if decoder.Decode(&raw) != nil || !ir.ValidJSON(raw) {
		return nil, false
	}
	decoder = json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	token, err := decoder.Token()
	if err != nil {
		return nil, false
	}
	return parseJSONToken(decoder, token)
}
func parseJSONToken(decoder *json.Decoder, token json.Token) (*jsonValue, bool) {
	switch value := token.(type) {
	case json.Delim:
		if value == '{' {
			node := &jsonValue{kind: jsonObject}
			for decoder.More() {
				key, err := decoder.Token()
				if err != nil {
					return nil, false
				}
				text, ok := key.(string)
				if !ok {
					return nil, false
				}
				token, err := decoder.Token()
				if err != nil {
					return nil, false
				}
				child, ok := parseJSONToken(decoder, token)
				if !ok {
					return nil, false
				}
				node.keys = append(node.keys, text)
				node.vals = append(node.vals, child)
			}
			token, err := decoder.Token()
			return node, err == nil && token == json.Delim('}')
		}
		if value == '[' {
			node := &jsonValue{kind: jsonArray}
			for decoder.More() {
				token, err := decoder.Token()
				if err != nil {
					return nil, false
				}
				child, ok := parseJSONToken(decoder, token)
				if !ok {
					return nil, false
				}
				node.items = append(node.items, child)
			}
			token, err := decoder.Token()
			return node, err == nil && token == json.Delim(']')
		}
	case string:
		return &jsonValue{kind: jsonString, str: value}, true
	case json.Number:
		number, _ := value.Float64()
		return &jsonValue{kind: jsonNumber, num: number, numberLexeme: string(value)}, true
	case bool:
		return &jsonValue{kind: jsonBool, boolean: value}, true
	case nil:
		return &jsonValue{kind: jsonNull}, true
	}
	return nil, false
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
		if v.numberLexeme != "" {
			b.WriteString(v.numberLexeme)
		} else {
			b.WriteString(formatCJSONNumber(v.num))
		}
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
	if parsed, ok := parseJSONPrefix(value); ok && ir.ValidJSON([]byte(trimC(value))) {
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
