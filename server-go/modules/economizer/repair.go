package economizer

// Tool-pairing repair for a message history.
//
// Ported from src/server/agent_bridge.c (message_history_repair and its
// collect_/detect_/repair_orphans_* helpers) plus
// src/server/tool_call_args.c (tool_call_sanitize_assistant_arguments).
//
// WHAT IT IS FOR: every provider rejects a transcript in which a tool call has
// no matching result, or a result refers to a call that is not there. Folding
// and compaction both retire messages, so either side of a pair can go missing;
// this puts the transcript back into a shape a provider will accept, by
// synthesizing a cancellation result for an orphaned call and dropping a result
// whose call is gone.
//
// It also exists as the economizer's own structural CHECK: run over a copy, a
// non-zero return means "the reduced view was not well formed". That use is
// what GWShouldApply's StructuralCheck port is for.
//
// THREE WIRE SHAPES, deliberately not unified: OpenAI pairs role=tool messages
// to assistant tool_calls[].id; Anthropic pairs tool_result blocks inside a user
// message's content to tool_use blocks inside an assistant's; the Responses API
// pairs top-level function_call_output items to function_call items by call_id.
// A single abstraction over the three would have to invent a shape none of the
// providers actually speaks.

// cancelMsg is the synthetic result body. Byte-identical to the C constant: it
// reaches real transcripts, so changing it changes prompts (and therefore cache
// keys) for every conversation that ever lost a tool result.
const cancelMsg = "[Tool call was cancelled or timed out]"

// Message-format ids, matching detect_format's return values.
const (
	fmtOpenAI    = 0
	fmtAnthropic = 1
	fmtResponses = 2
)

// idSet is an insertion-ordered set of tool ids.
//
// Order is part of the behaviour, not an implementation detail: repairs are
// emitted while iterating this set, so it decides the order synthetic messages
// are appended, and that lands in the prompt bytes. The C used a cJSON object
// and relied on its insertion-ordered child list.
//
// DELIBERATE DIVERGENCE: the C added a key per occurrence, so an id appearing
// twice was iterated twice and could produce two synthetic results for one call.
// Lookups there used cJSON_GetObjectItem, which returns the first match, so the
// structure only ever behaved as a set on the READ side. Its own comment says
// "used as a set", so this dedupes on insert, which is that stated intent and
// strictly fewer bogus repairs.
type idSet struct {
	order []string
	seen  map[string]bool
}

func newIDSet() *idSet { return &idSet{seen: map[string]bool{}} }

// add records an id. The EMPTY id is a legitimate member, not a sentinel: the C
// keyed its set with cJSON_AddBoolToObject, which accepts "" as a key, so an
// empty call id and an empty result id pair with each other, and an empty result
// id with no matching call is an orphan to be removed. Absent-vs-empty is
// decided by the caller via stringField, never by comparing to "".
func (s *idSet) add(id string) {
	if s.seen[id] {
		return
	}
	s.seen[id] = true
	s.order = append(s.order, id)
}

func (s *idSet) has(id string) bool { return s.seen[id] }

func (s *idSet) empty() bool { return len(s.order) == 0 }

// stringField reads a string field, reporting whether it was PRESENT as a string
// at all. That distinction is load-bearing and easy to lose in Go: the C tested
// cJSON_GetStringValue(...) against NULL, which separates "no such field" from
// "field is the empty string", while GetString collapses both to "". Using
// `!= ""` as the proxy silently keeps a role=tool message whose tool_call_id is
// "" -- a result that can never pair with a call, which every provider rejects.
func stringField(v *JSONValue, key string) (string, bool) {
	child := v.Get(key)
	if !child.IsString() {
		return "", false
	}
	return child.Str, true
}

// collectToolCallIDs gathers every id a tool CALL was issued under, across all
// three wire shapes. A message is inspected for all of them rather than only the
// detected format's, exactly as the C did: detection picks the repair strategy,
// not what counts as a call.
func collectToolCallIDs(messages *JSONValue) *idSet {
	ids := newIDSet()
	for _, msg := range messages.Items {
		if msg.GetString("role") == "assistant" {
			// OpenAI: assistant message with a tool_calls array.
			if tcs := msg.Get("tool_calls"); tcs.IsArray() {
				for _, tc := range tcs.Items {
					if id, ok := stringField(tc, "id"); ok {
						ids.add(id)
					}
				}
			}
			// Anthropic: tool_use blocks inside the content array.
			if content := msg.Get("content"); content.IsArray() {
				for _, block := range content.Items {
					if block.GetString("type") == "tool_use" {
						if id, ok := stringField(block, "id"); ok {
							ids.add(id)
						}
					}
				}
			}
		}
		// Responses API: a top-level function_call item, which carries no role.
		if msg.GetString("type") == "function_call" {
			if id, ok := stringField(msg, "call_id"); ok {
				ids.add(id)
			}
		}
	}
	return ids
}

// collectToolResultIDs gathers every id a tool RESULT was returned for.
func collectToolResultIDs(messages *JSONValue) *idSet {
	ids := newIDSet()
	for _, msg := range messages.Items {
		switch msg.GetString("role") {
		case "tool": // OpenAI
			if id, ok := stringField(msg, "tool_call_id"); ok {
				ids.add(id)
			}
		case "user": // Anthropic: tool_result blocks inside content
			if content := msg.Get("content"); content.IsArray() {
				for _, block := range content.Items {
					if block.GetString("type") == "tool_result" {
						if id, ok := stringField(block, "tool_use_id"); ok {
							ids.add(id)
						}
					}
				}
			}
		}
		if msg.GetString("type") == "function_call_output" {
			if id, ok := stringField(msg, "call_id"); ok {
				ids.add(id)
			}
		}
	}
	return ids
}

// detectFormat classifies the transcript: the FIRST message that matches any
// rule decides, so the checks stay in one pass in the C's order. Unknown shapes
// default to OpenAI, which is the broadest.
func detectFormat(messages *JSONValue) int {
	for _, msg := range messages.Items {
		if t := msg.GetString("type"); t == "function_call" || t == "function_call_output" {
			return fmtResponses
		}
		role := msg.GetString("role")
		if role == "" {
			continue
		}
		if role == "assistant" {
			if content := msg.Get("content"); content.IsArray() {
				for _, block := range content.Items {
					if block.GetString("type") == "tool_use" {
						return fmtAnthropic
					}
				}
			}
		}
		if role == "tool" {
			return fmtOpenAI
		}
	}
	return fmtOpenAI
}

// repairOrphansOpenAI inserts a synthetic role=tool result for each unanswered
// call, and drops results whose call is gone.
func repairOrphansOpenAI(messages *JSONValue, callIDs, resultIDs *idSet) int {
	repairs := 0

	for _, id := range callIDs.order {
		if resultIDs.has(id) {
			continue
		}

		// Place the result directly after the assistant message that issued the
		// call, and after any results already sitting behind it, so an existing
		// call/result run stays contiguous.
		insertAt := -1
		for i, msg := range messages.Items {
			if msg.GetString("role") != "assistant" {
				continue
			}
			tcs := msg.Get("tool_calls")
			if !tcs.IsArray() {
				continue
			}
			for _, tc := range tcs.Items {
				if tc.GetString("id") == id {
					insertAt = i + 1
					break
				}
			}
			if insertAt >= 0 {
				break
			}
		}

		toolMsg := NewObject()
		toolMsg.Set("role", NewString("tool"))
		toolMsg.Set("tool_call_id", NewString(id))
		toolMsg.Set("content", NewString(cancelMsg))

		if insertAt < 0 {
			messages.Append(toolMsg)
		} else {
			for insertAt < messages.Len() && messages.At(insertAt).GetString("role") == "tool" {
				insertAt++
			}
			messages.Insert(insertAt, toolMsg)
		}
		repairs++
	}

	// Drop results with no surviving call. Iterating backwards keeps the indices
	// of the not-yet-visited elements valid as entries are removed.
	for i := messages.Len() - 1; i >= 0; i-- {
		msg := messages.At(i)
		if msg.GetString("role") != "tool" {
			continue
		}
		if tcid, ok := stringField(msg, "tool_call_id"); ok && !callIDs.has(tcid) {
			messages.RemoveAt(i)
			repairs++
		}
	}

	return repairs
}

// repairOrphansAnthropic adds a tool_result block for each unanswered tool_use,
// preferring the next user message after the call so the pairing stays adjacent.
func repairOrphansAnthropic(messages *JSONValue, callIDs, resultIDs *idSet) int {
	repairs := 0

	for _, id := range callIDs.order {
		if resultIDs.has(id) {
			continue
		}

		// Walk once: find the assistant message carrying this tool_use, then the
		// first user message after it that has a content array to append to.
		foundCall := false
		var targetUser *JSONValue
		for _, msg := range messages.Items {
			if !foundCall {
				if msg.GetString("role") != "assistant" {
					continue
				}
				content := msg.Get("content")
				if !content.IsArray() {
					continue
				}
				for _, block := range content.Items {
					if block.GetString("type") == "tool_use" && block.GetString("id") == id {
						foundCall = true
						break
					}
				}
				continue
			}
			if msg.GetString("role") == "user" {
				if content := msg.Get("content"); content.IsArray() {
					targetUser = msg
					break
				}
			}
		}

		tr := NewObject()
		tr.Set("type", NewString("tool_result"))
		tr.Set("tool_use_id", NewString(id))
		tr.Set("content", NewString(cancelMsg))

		if targetUser != nil {
			targetUser.Get("content").Append(tr)
		} else {
			userMsg := NewObject()
			userMsg.Set("role", NewString("user"))
			content := NewArray()
			content.Append(tr)
			userMsg.Set("content", content)
			messages.Append(userMsg)
		}
		repairs++
	}

	// Drop tool_result blocks whose tool_use is gone, then drop any user message
	// left with an empty content array. That emptied-message removal is NOT
	// counted as a repair, matching the C: the repair was the block removal, and
	// double-counting it would inflate the structural check's verdict.
	for i := messages.Len() - 1; i >= 0; i-- {
		msg := messages.At(i)
		if msg.GetString("role") != "user" {
			continue
		}
		content := msg.Get("content")
		if !content.IsArray() {
			continue
		}
		for j := content.Len() - 1; j >= 0; j-- {
			block := content.At(j)
			if block.GetString("type") != "tool_result" {
				continue
			}
			if tuid, ok := stringField(block, "tool_use_id"); ok && !callIDs.has(tuid) {
				content.RemoveAt(j)
				repairs++
			}
		}
		if content.Len() == 0 {
			messages.RemoveAt(i)
		}
	}

	return repairs
}

// repairOrphansResponses appends a function_call_output for each unanswered
// function_call, and drops outputs whose call is gone.
func repairOrphansResponses(messages *JSONValue, callIDs, resultIDs *idSet) int {
	repairs := 0

	for _, id := range callIDs.order {
		if resultIDs.has(id) {
			continue
		}
		out := NewObject()
		out.Set("type", NewString("function_call_output"))
		out.Set("call_id", NewString(id))
		out.Set("output", NewString(cancelMsg))
		messages.Append(out)
		repairs++
	}

	for i := messages.Len() - 1; i >= 0; i-- {
		msg := messages.At(i)
		if msg.GetString("type") != "function_call_output" {
			continue
		}
		if cid, ok := stringField(msg, "call_id"); ok && !callIDs.has(cid) {
			messages.RemoveAt(i)
			repairs++
		}
	}

	return repairs
}

// SanitizeAssistantToolArguments coerces every tool_calls[].function.arguments
// on an assistant message to a parseable JSON string.
//
// Strict OpenAI-compatible providers (MiniMax in particular) reject the whole
// request with HTTP 400 "invalid function arguments json string" when arguments
// is missing, an object rather than a string, or a string that does not parse.
// Anything not already valid becomes "{}" — a request that drops one call's
// arguments still runs, where a rejected request loses the entire turn.
func SanitizeAssistantToolArguments(msg *JSONValue) {
	toolCalls := msg.Get("tool_calls")
	if !toolCalls.IsArray() {
		return
	}
	for _, tc := range toolCalls.Items {
		fn := tc.Get("function")
		if fn == nil {
			continue
		}
		args := fn.Get("arguments")
		if args.IsString() && ParseJSON(args.Str) != nil {
			continue
		}
		fn.Delete("arguments")
		fn.Set("arguments", NewString("{}"))
	}
}

// MessageHistoryRepair puts a transcript back into a shape a provider accepts,
// returning the number of repairs made. Zero means it was already well formed,
// which is what makes this usable as a structural check over a copy.
//
// Idempotent: repairing an already-repaired array returns 0.
func MessageHistoryRepair(messages *JSONValue) int {
	if !messages.IsArray() || messages.Len() == 0 {
		return 0
	}

	// Runs every turn, on every assistant message, independently of tool pairing:
	// a transcript can be perfectly paired and still be rejected for an
	// unparseable arguments string.
	for _, msg := range messages.Items {
		if msg.GetString("role") == "assistant" {
			SanitizeAssistantToolArguments(msg)
		}
	}

	callIDs := collectToolCallIDs(messages)
	resultIDs := collectToolResultIDs(messages)
	if callIDs.empty() && resultIDs.empty() {
		return 0
	}

	switch detectFormat(messages) {
	case fmtAnthropic:
		return repairOrphansAnthropic(messages, callIDs, resultIDs)
	case fmtResponses:
		return repairOrphansResponses(messages, callIDs, resultIDs)
	default:
		return repairOrphansOpenAI(messages, callIDs, resultIDs)
	}
}
