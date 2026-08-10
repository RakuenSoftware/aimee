package delegates

import "strings"

// parseXMLBlocks reads <tool_call> blocks, including namespaced ones, and falls
// back to Qwen's <function=> spelling inside a block.
func parseXMLBlocks(scan string, known knownTools, out *RescueResult) int {
	found := 0
	p := 0
	for !out.full() {
		blockStart, blockEnd, ok := findXMLTag(scan[p:], "tool_call")
		namespaced := false
		if !ok {
			blockStart, blockEnd, ok = findNamespacedToolCall(scan[p:])
			namespaced = ok
		}
		if !ok {
			break
		}
		blockStart += p
		blockEnd += p

		// find*XMLTag scans the whole remaining text, so a <name> or
		// <arguments> that closes past THIS block belongs to a later one.
		// Attributing it here would fabricate a call mixing one block's name
		// with another's arguments.
		nameStart, nameEnd, nameOK := findXMLTag(scan[blockStart:], "name")
		if nameOK {
			nameStart += blockStart
			nameEnd += blockStart
			if nameEnd > blockEnd {
				nameOK = false
			}
		}
		argsStart, argsEnd, argsOK := findXMLTag(scan[blockStart:], "arguments")
		if argsOK {
			argsStart += blockStart
			argsEnd += blockStart
			if argsEnd > blockEnd {
				argsOK = false
			}
		}

		// A present tag is not a present name. An empty <name></name> once
		// produced a call named "", which dispatches to nothing and is
		// indistinguishable downstream from a tool that does not exist; and a
		// name too long for the field was truncated, so two distinct long names
		// collapsed to one prefix and a call could be attributed to the WRONG
		// tool. Refuse instead of guessing.
		rawNameLen := 0
		if nameOK {
			rawNameLen = nameEnd - nameStart
		}
		nameUsable := nameOK && rawNameLen <= rescueNameMax

		if nameUsable {
			name := normalizeToolName(trimC(scan[nameStart:nameEnd]))
			if name == "" {
				// Nothing survived trimming: leave the block unparsed rather
				// than emit a nameless call.
				p = advanceBlock(scan, blockEnd, namespaced)
				continue
			}
			out.appendCall(name, xmlArguments(scan, argsStart, argsEnd, argsOK))
			found++
		} else if parseQwenFunctionCall(scan[blockStart:blockEnd], out) {
			found++
		}

		p = advanceBlock(scan, blockEnd, namespaced)
		if p > len(scan) {
			break
		}
	}
	return found
}

// xmlArguments keeps the arguments verbatim when they are JSON, and otherwise
// wraps them so the executor still receives an object.
func xmlArguments(scan string, start, end int, ok bool) string {
	if !ok {
		return "{}"
	}
	args := trimC(scan[start:end])
	if _, valid := parseJSONPrefix(args); valid {
		return args
	}
	obj := &orderedObject{}
	obj.addParameterValue("value", args)
	// addParameterValue would re-parse; force the string form.
	return printJSON(&jsonValue{
		kind: jsonObject,
		keys: []string{"value"},
		vals: []*jsonValue{{kind: jsonString, str: args}},
	})
}

// advanceBlock steps past a closing tag. A namespaced close is longer than the
// plain one, so find its '>' rather than assuming a width.
func advanceBlock(scan string, blockEnd int, namespaced bool) int {
	if namespaced {
		if gt := strings.IndexByte(scan[blockEnd:], '>'); gt >= 0 {
			return blockEnd + gt + 1
		}
		return blockEnd + len("</tool_call>")
	}
	return blockEnd + len("</tool_call>")
}

// parseQwenFunctionCall reads <function=name><parameter=key>value</parameter>.
func parseQwenFunctionCall(block string, out *RescueResult) bool {
	fn := strings.Index(block, "<function=")
	if fn < 0 {
		return false
	}
	nameStart := fn + len("<function=")
	gt := strings.IndexByte(block[nameStart:], '>')
	if gt < 0 {
		return false
	}
	nameEnd := nameStart + gt
	name := normalizeToolName(trimC(truncName(block[nameStart:nameEnd])))

	bodyStart := nameEnd + 1
	bodyEnd := len(block)
	if i := strings.Index(block[bodyStart:], "</function>"); i >= 0 {
		bodyEnd = bodyStart + i
	}

	args := &orderedObject{}
	p := bodyStart
	for p < bodyEnd {
		param := strings.Index(block[p:bodyEnd], "<parameter=")
		if param < 0 {
			break
		}
		keyStart := p + param + len("<parameter=")
		kgt := strings.IndexByte(block[keyStart:], '>')
		if kgt < 0 || keyStart+kgt > bodyEnd {
			break
		}
		keyEnd := keyStart + kgt
		valueStart := keyEnd + 1
		ve := strings.Index(block[valueStart:bodyEnd], "</parameter>")
		if ve < 0 {
			break
		}
		valueEnd := valueStart + ve
		args.addParameterValue(trimC(block[keyStart:keyEnd]), trimC(block[valueStart:valueEnd]))
		p = valueEnd + len("</parameter>")
	}

	out.appendCall(name, args.print())
	return true
}

// parseInvokeCall reads <invoke name="x"><parameter name="k">v</parameter>.
func parseInvokeCall(block string, known knownTools, out *RescueResult) bool {
	invoke := strings.Index(block, "<invoke")
	if invoke < 0 {
		return false
	}
	tagEnd := strings.Index(block[invoke:], ">")
	if tagEnd < 0 {
		return false
	}
	tagEnd += invoke
	name, ok := xmlAttrValue(block[invoke:tagEnd], "name")
	if !ok || name == "" {
		return false
	}
	name = normalizeToolName(name)
	if !known.known(name) {
		return false
	}

	bodyStart := tagEnd + 1
	bodyEnd := len(block)
	if i := strings.Index(block[bodyStart:], "</invoke>"); i >= 0 {
		bodyEnd = bodyStart + i
	}

	args := &orderedObject{}
	p := bodyStart
	for p < bodyEnd {
		param := strings.Index(block[p:bodyEnd], "<parameter")
		if param < 0 {
			break
		}
		paramAt := p + param
		pe := strings.Index(block[paramAt:bodyEnd], ">")
		if pe < 0 {
			break
		}
		paramTagEnd := paramAt + pe
		key, _ := xmlAttrValue(block[paramAt:paramTagEnd], "name")
		valueStart := paramTagEnd + 1
		ve := strings.Index(block[valueStart:bodyEnd], "</parameter>")
		if ve < 0 {
			break
		}
		valueEnd := valueStart + ve
		args.addParameterValue(key, trimC(block[valueStart:valueEnd]))
		p = valueEnd + len("</parameter>")
	}

	out.appendCall(truncName(name), args.print())
	return true
}

func parseInvokeCalls(scan string, known knownTools, out *RescueResult) int {
	found := 0
	p := 0
	for !out.full() {
		i := strings.Index(scan[p:], "<invoke")
		if i < 0 {
			break
		}
		invoke := p + i
		gt := strings.IndexByte(scan[invoke:], '>')
		if gt < 0 {
			break
		}
		tagEnd := invoke + gt
		blockEnd := tagEnd + 1
		if c := strings.Index(scan[tagEnd+1:], "</invoke>"); c >= 0 {
			blockEnd = tagEnd + 1 + c + len("</invoke>")
		}
		before := len(out.Calls)
		if parseInvokeCall(scan[invoke:blockEnd], known, out) {
			found++
			out.setContentIfEmpty(scan[:invoke])
		}
		if len(out.Calls) == before {
			p = tagEnd + 1
		} else {
			p = blockEnd
		}
	}
	if found > 0 {
		out.IsToolCall = true
	}
	return found
}

// decodeChannelArgText restores quotes the harmony channel encodes as <|"|>.
func decodeChannelArgText(s string) string {
	const quoteMarker = `<|"|>`
	var b strings.Builder
	for i := 0; i < len(s); {
		if strings.HasPrefix(s[i:], quoteMarker) {
			b.WriteByte('"')
			i += len(quoteMarker)
			continue
		}
		b.WriteByte(s[i])
		i++
	}
	return trimC(b.String())
}

// channelArgsToJSON takes the INSIDE of the braces and returns an object.
func channelArgsToJSON(inside string) string {
	decoded := decodeChannelArgText(inside)

	// Put the braces back before asking whether this is already an arguments
	// object. Parsing the brace-less text is a different question: a bare
	// `"command": "ls"` parses as the leading string and stops, so the guard
	// passed and a JSON *string* was handed to an executor wanting an object.
	wrapped := "{" + decoded + "}"
	if v, ok := parseJSONPrefix(wrapped); ok && v.isObject() {
		return wrapped
	}

	if colon := strings.IndexByte(decoded, ':'); colon >= 0 {
		key := trimC(decoded[:colon])
		value := trimC(decoded[colon+1:])
		if len(value) >= 2 && value[0] == '"' && value[len(value)-1] == '"' {
			value = value[1 : len(value)-1]
		}
		if key == "" {
			key = "value"
		}
		return printJSON(&jsonValue{
			kind: jsonObject,
			keys: []string{key},
			vals: []*jsonValue{{kind: jsonString, str: value}},
		})
	}
	return printJSON(&jsonValue{
		kind: jsonObject,
		keys: []string{"value"},
		vals: []*jsonValue{{kind: jsonString, str: decoded}},
	})
}

func parseChannelCalls(scan string, out *RescueResult) int {
	const marker = "<|channel>call:"
	found := 0
	p := 0
	for !out.full() {
		i := strings.Index(scan[p:], marker)
		if i < 0 {
			break
		}
		start := p + i
		nameStart := start + len(marker)
		for nameStart < len(scan) && isCSpace(scan[nameStart]) {
			nameStart++
		}
		nameEnd := nameStart
		for nameEnd < len(scan) && scan[nameEnd] != '{' && !isCSpace(scan[nameEnd]) &&
			scan[nameEnd] != '<' {
			nameEnd++
		}
		brace := strings.IndexByte(scan[nameEnd:], '{')
		if brace < 0 {
			break
		}
		argsOpen := nameEnd + brace
		argsClose := findJSONObjectEnd(scan, argsOpen)
		if argsClose < 0 {
			break
		}
		name := normalizeToolName(trimC(truncName(scan[nameStart:nameEnd])))
		out.appendCall(name, channelArgsToJSON(scan[argsOpen+1:argsClose]))
		found++
		p = argsClose + 1
	}
	if found > 0 {
		out.IsToolCall = true
		if first := strings.Index(scan, marker); first > 0 {
			out.setContentIfEmpty(scan[:first])
		}
	}
	return found
}

func parseMistralBracketCalls(scan string, known knownTools, out *RescueResult) int {
	const marker = "[TOOL_CALLS]"
	found := 0
	p := 0
	for !out.full() {
		i := strings.Index(scan[p:], marker)
		if i < 0 {
			break
		}
		start := p + i
		nameStart := start + len(marker)
		for nameStart < len(scan) && (isCSpace(scan[nameStart]) || scan[nameStart] == ',') {
			nameStart++
		}
		nameEnd := nameStart
		for nameEnd < len(scan) {
			c := scan[nameEnd]
			if isAlnum(c) || c == '_' || c == '-' || c == '.' || c == ':' {
				nameEnd++
				continue
			}
			break
		}
		if nameEnd == nameStart {
			break
		}
		argsOpen := nameEnd
		for argsOpen < len(scan) && isCSpace(scan[argsOpen]) {
			argsOpen++
		}
		if argsOpen >= len(scan) || scan[argsOpen] != '{' {
			break
		}
		argsClose := findJSONObjectEnd(scan, argsOpen)
		if argsClose < 0 {
			break
		}
		name := trimC(truncName(scan[nameStart:nameEnd]))
		if !known.known(name) {
			p = argsClose + 1
			continue
		}
		out.appendCall(normalizeToolName(trimC(truncName(name))),
			fillArguments(scan[argsOpen:argsClose+1]))
		found++
		p = argsClose + 1
	}
	if found > 0 {
		out.IsToolCall = true
		if first := strings.Index(scan, marker); first > 0 {
			out.setContentIfEmpty(scan[:first])
		}
	}
	return found
}

// fillArguments mirrors fill_tool_call's argument handling: trimmed, and an
// empty result becomes an empty object rather than nothing.
func fillArguments(args string) string {
	trimmed := trimC(args)
	if trimmed == "" {
		return "{}"
	}
	return trimmed
}

// jsonArgumentsFromItem mirrors json_arguments_from_item: an object is
// re-printed, a string holding an object is unwrapped, anything else is empty.
func jsonArgumentsFromItem(item *jsonValue) string {
	if item == nil {
		return "{}"
	}
	if item.isObject() {
		return printJSON(item)
	}
	if item.isString() {
		if parsed, ok := parseJSONPrefix(item.str); ok && parsed.isObject() {
			return printJSON(parsed)
		}
	}
	return "{}"
}

// parseJSONToolObject reads {"name":...,"arguments":...} and its spellings.
func parseJSONToolObject(text string, known knownTools, out *RescueResult) bool {
	if out.full() {
		return false
	}
	root, ok := parseJSONPrefix(trimC(text))
	if !ok || !root.isObject() {
		return false
	}
	jname := root.get("tool")
	if !jname.isString() {
		jname = root.get("name")
	}
	if !jname.isString() || !known.known(jname.str) {
		return false
	}

	// Every spelling whose NAME key is accepted above must have its ARGUMENTS
	// key accepted too. "tool" exists for the tool/parameters convention, but
	// "parameters" went unread, so such a call was invoked with an EMPTY
	// argument object -- worse than declining it, because bash then runs with
	// no command instead of being left alone.
	jargs := root.get("args")
	if jargs == nil {
		jargs = root.get("arguments")
	}
	if jargs == nil {
		jargs = root.get("parameters")
	}

	out.appendCall(truncName(jname.str), jsonArgumentsFromItem(jargs))
	return true
}

func parseBareJSONCalls(scan string, known knownTools, out *RescueResult) int {
	found := 0
	p := 0
	for !out.full() {
		open := strings.IndexByte(scan[p:], '{')
		if open < 0 {
			break
		}
		open += p
		close := findJSONObjectEnd(scan, open)
		if close < 0 {
			break
		}
		before := len(out.Calls)
		if parseJSONToolObject(scan[open:close+1], known, out) {
			found++
			out.setContentIfEmpty(scan[:open])
		}
		if len(out.Calls) == before {
			p = open + 1
		} else {
			p = close + 1
		}
	}
	if found > 0 {
		out.IsToolCall = true
	}
	return found
}

// RescueParseToolCalls recovers tool calls a model wrote as text. allowJSON
// controls only the bare-JSON dialect; the tagged ones are always tried.
func RescueParseToolCalls(text string, knownNames []string, allowJSON bool) (RescueResult, int) {
	var out RescueResult
	if text == "" {
		return out, 0
	}
	known := newKnownTools(knownNames)
	scan := stripReasoningBlocks(text)

	found := parseXMLBlocks(scan, known, &out)

	if found == 0 {
		p := 0
		for !out.full() {
			blockStart, blockEnd, closeLen, ok := findLocalXMLTag(scan[p:], "tool_call")
			if !ok {
				break
			}
			blockStart += p
			blockEnd += p
			if parseInvokeCall(scan[blockStart:blockEnd], known, &out) {
				found++
			}
			p = blockEnd + closeLen
		}
	}

	if found > 0 {
		out.IsToolCall = true
		first := strings.Index(scan, "<tool_call>")
		if first < 0 {
			if ns := strings.Index(scan, ":tool_call>"); ns >= 0 {
				for ns > 0 && scan[ns] != '<' {
					ns--
				}
				if ns >= 0 && ns < len(scan) && scan[ns] == '<' {
					first = ns
				}
			}
		}
		if first > 0 {
			out.setContentIfEmpty(scan[:first])
		}
	}

	// One dialect per response: the first that yields anything wins.
	if found == 0 {
		found = parseChannelCalls(scan, &out)
	}
	if found == 0 {
		found = parseInvokeCalls(scan, known, &out)
	}
	if found == 0 {
		found = parseMistralBracketCalls(scan, known, &out)
	}
	if found == 0 && allowJSON {
		found = parseBareJSONCalls(scan, known, &out)
	}
	return out, found
}

// RescueHasToolCalls reports whether text looks like it carries a rescued call.
// The tagged markers are a cheap literal check; bare JSON has to be parsed,
// because only a known tool name makes an object a call.
func RescueHasToolCalls(text string, knownNames []string, allowJSON bool) bool {
	if text == "" {
		return false
	}
	if strings.Contains(text, "<tool_call>") || strings.Contains(text, "<invoke") ||
		strings.Contains(text, "<|channel>call:") || strings.Contains(text, "[TOOL_CALLS]") ||
		strings.Contains(text, ":tool_call>") {
		return true
	}
	if !allowJSON {
		return false
	}
	known := newKnownTools(knownNames)
	scan := stripReasoningBlocks(text)
	p := 0
	for {
		open := strings.IndexByte(scan[p:], '{')
		if open < 0 {
			return false
		}
		open += p
		close := findJSONObjectEnd(scan, open)
		if close < 0 {
			return false
		}
		var probe RescueResult
		if parseJSONToolObject(scan[open:close+1], known, &probe) {
			return true
		}
		p = open + 1
	}
}
