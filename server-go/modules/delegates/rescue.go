package delegates

import (
	"strconv"
	"strings"
)

// Recovering tool calls from a model that did not make one properly.
//
// Some models answer with tool calls written into the prose instead of through
// the provider's tool-call channel, and each family invented its own spelling:
// <tool_call> blocks, namespaced variants, Qwen's <function=>, Anthropic-style
// <invoke>, harmony's <|channel>call:, Mistral's [TOOL_CALLS], and bare JSON.
// This reads all of them and produces the same structure the native path does.
//
// The dialects are tried in a fixed order and the FIRST that yields anything
// wins; a response is written in one dialect, so a later parser matching too is
// a coincidence, not a second call.

const (
	// Mirrors AGENT_MAX_TOOL_CALLS.
	rescueMaxToolCalls = 16
	// Mirrors parsed_tool_call_t.name (char[32]): 31 usable bytes.
	rescueNameMax = 31
)

// RescueToolCall mirrors parsed_tool_call_t.
type RescueToolCall struct {
	ID        string
	Name      string
	Arguments string
}

// RescueResult carries the fields the rescue parser fills in
// parsed_response_t. Content is empty when there was no leading prose.
type RescueResult struct {
	IsToolCall bool
	Content    string
	Calls      []RescueToolCall
}

// knownTools is the caller's tool inventory, passed in with the request.
//
// The rescue needs to know whether a name refers to a real tool, but the tool
// registry belongs to another module and a module may not call another module
// directly. So the caller sends the names it will accept; they are input this
// stage reads and forgets, not state it keeps.
type knownTools map[string]bool

func newKnownTools(names []string) knownTools {
	set := make(knownTools, len(names))
	for _, name := range names {
		set[name] = true
	}
	return set
}

// known reports whether a rescued name may be dispatched. A namespaced name
// (tool:sub) is accepted without an inventory entry because the namespace is
// resolved later, and "respond" is always available.
func (k knownTools) known(name string) bool {
	return name != "" && (name == "respond" || strings.Contains(name, ":") || k[name])
}

func isCSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

func trimC(s string) string {
	start := 0
	for start < len(s) && isCSpace(s[start]) {
		start++
	}
	end := len(s)
	for end > start && isCSpace(s[end-1]) {
		end--
	}
	return s[start:end]
}

// truncName mirrors the fixed-width name field: the copy is bounded before the
// trim, so trailing space inside the limit still counts against it.
func truncName(s string) string {
	if len(s) > rescueNameMax {
		s = s[:rescueNameMax]
	}
	return s
}

// normalizeToolName folds the spellings models emit onto the registered name.
func normalizeToolName(name string) string {
	if name == "" {
		return name
	}
	if name == "Bash" {
		return "bash"
	}
	out := []byte(name)
	for i, c := range out {
		switch {
		case c == '-':
			out[i] = '_'
		case c >= 'A' && c <= 'Z':
			out[i] = c + ('a' - 'A')
		}
	}
	return string(out)
}

func indexCI(haystack, needle string) int {
	if haystack == "" || needle == "" {
		return -1
	}
	if len(needle) > len(haystack) {
		return -1
	}
	for i := 0; i+len(needle) <= len(haystack); i++ {
		match := true
		for j := 0; j < len(needle); j++ {
			a, b := haystack[i+j], needle[j]
			if a >= 'A' && a <= 'Z' {
				a += 'a' - 'A'
			}
			if b >= 'A' && b <= 'Z' {
				b += 'a' - 'A'
			}
			if a != b {
				match = false
				break
			}
		}
		if match {
			return i
		}
	}
	return -1
}

// removeReasoningBlock deletes each open..close span. An unclosed opener takes
// the rest of the text with it: the model was still thinking when it stopped,
// so nothing after it is an answer.
func removeReasoningBlock(s, openTag, closeTag string) string {
	for {
		open := indexCI(s, openTag)
		if open < 0 {
			return s
		}
		rest := s[open+len(openTag):]
		close := indexCI(rest, closeTag)
		if close < 0 {
			return s[:open]
		}
		s = s[:open] + rest[close+len(closeTag):]
	}
}

func stripReasoningBlocks(text string) string {
	s := removeReasoningBlock(text, "<think>", "</think>")
	s = removeReasoningBlock(s, "[THINK]", "[/THINK]")
	return trimC(s)
}

// findXMLTag returns the span between <tag> and </tag>, searching the whole
// remaining text.
func findXMLTag(s, tag string) (start, end int, ok bool) {
	open := "<" + tag + ">"
	close := "</" + tag + ">"
	i := strings.Index(s, open)
	if i < 0 {
		return 0, 0, false
	}
	start = i + len(open)
	j := strings.Index(s[start:], close)
	if j < 0 {
		return 0, 0, false
	}
	return start, start + j, true
}

// findNamespacedToolCall matches <ns:tool_call>. The detector already reports
// these as tool calls, so a scanner that only knew the bare tag would promise
// work and deliver none.
func findNamespacedToolCall(s string) (start, end int, ok bool) {
	marker := strings.Index(s, ":tool_call>")
	if marker < 0 {
		return 0, 0, false
	}
	open := marker
	for open > 0 && s[open-1] != '<' {
		c := s[open-1]
		if !(isAlnum(c) || c == '_' || c == '-' || c == '.') {
			return 0, 0, false
		}
		open--
	}
	if open == 0 || s[open-1] != '<' {
		return 0, 0, false
	}
	prefixLen := marker - open
	if prefixLen == 0 || prefixLen > 32 {
		return 0, 0, false
	}
	closeTag := "</" + s[open:marker] + ":tool_call>"
	body := marker + len(":tool_call>")
	j := strings.Index(s[body:], closeTag)
	if j < 0 {
		return 0, 0, false
	}
	return body, body + j, true
}

func isAlnum(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}

// findLocalXMLTag matches a tag by LOCAL name, ignoring any namespace prefix.
func findLocalXMLTag(s, localName string) (start, end, closeLen int, ok bool) {
	for p := 0; p < len(s); p++ {
		lt := strings.IndexByte(s[p:], '<')
		if lt < 0 {
			return 0, 0, 0, false
		}
		p += lt
		if p+1 >= len(s) {
			return 0, 0, 0, false
		}
		if s[p+1] == '/' || s[p+1] == '!' || s[p+1] == '?' {
			continue
		}
		tagStart := p + 1
		tagEnd := tagStart
		for tagEnd < len(s) && s[tagEnd] != '>' && !isCSpace(s[tagEnd]) {
			tagEnd++
		}
		if tagEnd >= len(s) {
			return 0, 0, 0, false
		}
		localStart := tagStart
		for q := tagStart; q < tagEnd; q++ {
			if s[q] == ':' {
				localStart = q + 1
			}
		}
		if tagEnd-localStart != len(localName) || s[localStart:tagEnd] != localName {
			continue
		}
		gt := strings.IndexByte(s[tagEnd:], '>')
		if gt < 0 {
			return 0, 0, 0, false
		}
		openEnd := tagEnd + gt
		closeTag := "</" + s[tagStart:tagEnd] + ">"
		j := strings.Index(s[openEnd+1:], closeTag)
		if j < 0 {
			continue
		}
		return openEnd + 1, openEnd + 1 + j, len(closeTag), true
	}
	return 0, 0, 0, false
}

// xmlAttrValue reads attr="value" from within a tag.
func xmlAttrValue(tag, attr string) (string, bool) {
	p := 0
	for {
		i := strings.Index(tag[p:], attr)
		if i < 0 {
			return "", false
		}
		at := p + i
		if (at == 0 || isCSpace(tag[at-1])) && at+len(attr) < len(tag) && tag[at+len(attr)] == '=' {
			p = at
			break
		}
		p = at + len(attr)
	}
	p += len(attr)
	if p >= len(tag) || tag[p] != '=' {
		return "", false
	}
	p++
	for p < len(tag) && isCSpace(tag[p]) {
		p++
	}
	if p >= len(tag) || (tag[p] != '"' && tag[p] != '\'') {
		return "", false
	}
	quote := tag[p]
	p++
	start := p
	for p < len(tag) && tag[p] != quote {
		p++
	}
	if p >= len(tag) {
		return "", false
	}
	return trimC(tag[start:p]), true
}

// findJSONObjectEnd returns the index of the '}' closing the object at open,
// tracking strings so a brace inside a string does not close it.
func findJSONObjectEnd(s string, open int) int {
	if open >= len(s) || s[open] != '{' {
		return -1
	}
	depth := 0
	inString := false
	escaped := false
	for p := open; p < len(s); p++ {
		ch := s[p]
		if inString {
			switch {
			case escaped:
				escaped = false
			case ch == '\\':
				escaped = true
			case ch == '"':
				inString = false
			}
			continue
		}
		switch ch {
		case '"':
			inString = true
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return p
			}
		}
	}
	return -1
}

func (r *RescueResult) full() bool { return len(r.Calls) >= rescueMaxToolCalls }

func (r *RescueResult) appendCall(name, arguments string) {
	r.Calls = append(r.Calls, RescueToolCall{
		ID:        "xml_call_" + strconv.Itoa(len(r.Calls)+1),
		Name:      name,
		Arguments: arguments,
	})
}

// setContentIfEmpty records leading prose the model wrote before its first call.
func (r *RescueResult) setContentIfEmpty(pre string) {
	if r.Content != "" {
		return
	}
	if trimmed := trimC(pre); trimmed != "" {
		r.Content = trimmed
	}
}
