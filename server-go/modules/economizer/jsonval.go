package economizer

// Small accessors over JSONValue, so the ported fold reads like the C it came
// from (cJSON_GetObjectItem / cJSON_GetStringValue / cJSON_Duplicate) instead of
// open-coding index lookups at every call site.

// NewString builds a JSON string value.
func NewString(s string) *JSONValue { return &JSONValue{Kind: JSONString, Str: s} }

// NewNumber builds a JSON number value.
func NewNumber(n float64) *JSONValue { return &JSONValue{Kind: JSONNumber, Num: n} }

// NewObject builds an empty JSON object.
func NewObject() *JSONValue { return &JSONValue{Kind: JSONObject} }

// NewArray builds an empty JSON array.
func NewArray() *JSONValue { return &JSONValue{Kind: JSONArray} }

// Get returns the value for key, or nil. Case-sensitive, like the
// cJSON_GetObjectItemCaseSensitive the module uses throughout.
func (v *JSONValue) Get(key string) *JSONValue {
	if v == nil || v.Kind != JSONObject {
		return nil
	}
	for i, k := range v.Keys {
		if k == key {
			return v.Vals[i]
		}
	}
	return nil
}

// GetString returns the string at key, or "" when absent or not a string.
func (v *JSONValue) GetString(key string) string {
	child := v.Get(key)
	if child == nil || child.Kind != JSONString {
		return ""
	}
	return child.Str
}

// Set assigns key, appending it (preserving insertion order) or replacing in
// place if it already exists.
func (v *JSONValue) Set(key string, val *JSONValue) {
	if v == nil || v.Kind != JSONObject {
		return
	}
	for i, k := range v.Keys {
		if k == key {
			v.Vals[i] = val
			return
		}
	}
	v.Keys = append(v.Keys, key)
	v.Vals = append(v.Vals, val)
}

// Append adds an element to an array.
func (v *JSONValue) Append(item *JSONValue) {
	if v == nil || v.Kind != JSONArray {
		return
	}
	v.Items = append(v.Items, item)
}

// Insert places item at index i of an array, shifting the rest right. An i at or
// past the end appends; a negative i prepends.
//
// This is the slice equivalent of the pointer splice the C repair did by hand
// (tool_msg->next/prev rewiring around an insertion point). That splice is where
// a malformed ->next chain could be introduced by a single missed assignment;
// here the invariant is the slice's own.
func (v *JSONValue) Insert(i int, item *JSONValue) {
	if v == nil || v.Kind != JSONArray {
		return
	}
	if i < 0 {
		i = 0
	}
	if i >= len(v.Items) {
		v.Items = append(v.Items, item)
		return
	}
	v.Items = append(v.Items, nil)
	copy(v.Items[i+1:], v.Items[i:])
	v.Items[i] = item
}

// RemoveAt drops array element i. Out-of-range is a no-op.
func (v *JSONValue) RemoveAt(i int) {
	if v == nil || v.Kind != JSONArray || i < 0 || i >= len(v.Items) {
		return
	}
	v.Items = append(v.Items[:i], v.Items[i+1:]...)
}

// Delete removes key from an object, preserving the order of the rest. Mirrors
// cJSON_DeleteItemFromObjectCaseSensitive.
func (v *JSONValue) Delete(key string) {
	if v == nil || v.Kind != JSONObject {
		return
	}
	for i, k := range v.Keys {
		if k == key {
			v.Keys = append(v.Keys[:i], v.Keys[i+1:]...)
			v.Vals = append(v.Vals[:i], v.Vals[i+1:]...)
			return
		}
	}
}

// Len is the element count for an array, or the key count for an object.
func (v *JSONValue) Len() int {
	if v == nil {
		return 0
	}
	switch v.Kind {
	case JSONArray:
		return len(v.Items)
	case JSONObject:
		return len(v.Keys)
	}
	return 0
}

// At returns array element i, or nil when out of range.
func (v *JSONValue) At(i int) *JSONValue {
	if v == nil || v.Kind != JSONArray || i < 0 || i >= len(v.Items) {
		return nil
	}
	return v.Items[i]
}

// IsString reports whether v is a JSON string.
func (v *JSONValue) IsString() bool { return v != nil && v.Kind == JSONString }

// IsArray reports whether v is a JSON array.
func (v *JSONValue) IsArray() bool { return v != nil && v.Kind == JSONArray }

// Clone deep-copies v, mirroring cJSON_Duplicate(item, 1). The fold never
// mutates its input, so every published message is a copy.
func (v *JSONValue) Clone() *JSONValue {
	if v == nil {
		return nil
	}
	out := &JSONValue{Kind: v.Kind, Str: v.Str, Num: v.Num, Bool: v.Bool}
	if len(v.Keys) > 0 {
		out.Keys = append([]string(nil), v.Keys...)
		out.Vals = make([]*JSONValue, len(v.Vals))
		for i, child := range v.Vals {
			out.Vals[i] = child.Clone()
		}
	}
	if len(v.Items) > 0 {
		out.Items = make([]*JSONValue, len(v.Items))
		for i, child := range v.Items {
			out.Items[i] = child.Clone()
		}
	}
	return out
}

// Text renders v as the text a body carries: a string value yields its contents
// verbatim, anything else its serialized JSON.
//
// This mirrors the C pattern of using valuestring when the node is a string and
// cJSON_PrintUnformatted otherwise, which is why the printer has to be
// cJSON-compatible — the serialized form ends up inside the folded prefix.
func (v *JSONValue) Text() (string, bool) {
	if v == nil {
		return "", false
	}
	if v.Kind == JSONString {
		return v.Str, true
	}
	return PrintJSONUnformatted(v), true
}
