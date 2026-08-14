package economizer

import (
	"strings"
	"testing"
)

// Carried over one-for-one from src/tests/test_agent_repair.c, case for case and
// assertion for assertion, so this owner stays pinned to the behaviour it
// replaced. The C helper names are kept so the two files can be diffed by eye.

func makeMsg(role, content string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString(role))
	m.Set("content", NewString(content))
	return m
}

func makeAssistantWithToolsOpenAI(ids, names []string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("assistant"))
	m.Set("content", &JSONValue{Kind: JSONNull})
	tcs := NewArray()
	for i := range ids {
		tc := NewObject()
		tc.Set("id", NewString(ids[i]))
		tc.Set("type", NewString("function"))
		fn := NewObject()
		fn.Set("name", NewString(names[i]))
		fn.Set("arguments", NewString("{}"))
		tc.Set("function", fn)
		tcs.Append(tc)
	}
	m.Set("tool_calls", tcs)
	return m
}

func makeToolResultOpenAI(toolCallID, content string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("tool"))
	m.Set("tool_call_id", NewString(toolCallID))
	m.Set("content", NewString(content))
	return m
}

func makeAssistantWithToolsAnthropic(ids, names []string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("assistant"))
	content := NewArray()
	for i := range ids {
		block := NewObject()
		block.Set("type", NewString("tool_use"))
		block.Set("id", NewString(ids[i]))
		block.Set("name", NewString(names[i]))
		block.Set("input", NewObject())
		content.Append(block)
	}
	m.Set("content", content)
	return m
}

func makeToolResultsAnthropic(ids, contents []string) *JSONValue {
	m := NewObject()
	m.Set("role", NewString("user"))
	content := NewArray()
	for i := range ids {
		tr := NewObject()
		tr.Set("type", NewString("tool_result"))
		tr.Set("tool_use_id", NewString(ids[i]))
		tr.Set("content", NewString(contents[i]))
		content.Append(tr)
	}
	m.Set("content", content)
	return m
}

func arrayOf(items ...*JSONValue) *JSONValue {
	arr := NewArray()
	for _, it := range items {
		arr.Append(it)
	}
	return arr
}

func TestRepairEmpty(t *testing.T) {
	if got := MessageHistoryRepair(nil); got != 0 {
		t.Fatalf("nil: got %d, want 0", got)
	}
	if got := MessageHistoryRepair(NewArray()); got != 0 {
		t.Fatalf("empty array: got %d, want 0", got)
	}
}

func TestRepairNoTools(t *testing.T) {
	arr := arrayOf(
		makeMsg("system", "You are helpful."),
		makeMsg("user", "Hello"),
		makeMsg("assistant", "Hi there!"),
	)
	if got := MessageHistoryRepair(arr); got != 0 {
		t.Fatalf("got %d repairs, want 0", got)
	}
	if arr.Len() != 3 {
		t.Fatalf("len = %d, want 3", arr.Len())
	}
}

func TestRepairConsistentOpenAI(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "list files"),
		makeAssistantWithToolsOpenAI([]string{"call_1"}, []string{"bash"}),
		makeToolResultOpenAI("call_1", "file1.txt\nfile2.txt"),
		makeMsg("assistant", "Found 2 files."),
	)
	if got := MessageHistoryRepair(arr); got != 0 {
		t.Fatalf("got %d repairs, want 0", got)
	}
	if arr.Len() != 4 {
		t.Fatalf("len = %d, want 4", arr.Len())
	}
}

func TestRepairOrphanedCallOpenAI(t *testing.T) {
	// Assistant made a tool call but the result is missing (crash mid-execution).
	arr := arrayOf(
		makeMsg("user", "list files"),
		makeAssistantWithToolsOpenAI([]string{"call_orphan"}, []string{"bash"}),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	if arr.Len() != 3 {
		t.Fatalf("len = %d, want 3", arr.Len())
	}
	last := arr.At(2)
	if role := last.GetString("role"); role != "tool" {
		t.Fatalf("role = %q, want tool", role)
	}
	if id := last.GetString("tool_call_id"); id != "call_orphan" {
		t.Fatalf("tool_call_id = %q, want call_orphan", id)
	}
	if c := last.GetString("content"); !strings.Contains(c, "cancelled") {
		t.Fatalf("content = %q, want it to mention cancelled", c)
	}
}

func TestRepairOrphanedResultOpenAI(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "hello"),
		makeToolResultOpenAI("call_ghost", "some result"),
		makeMsg("assistant", "done"),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	if arr.Len() != 2 {
		t.Fatalf("len = %d, want 2 (orphaned result removed)", arr.Len())
	}
}

func TestRepairMultipleOrphansOpenAI(t *testing.T) {
	// Two calls, only one answered.
	arr := arrayOf(
		makeMsg("user", "do stuff"),
		makeAssistantWithToolsOpenAI([]string{"call_a", "call_b"}, []string{"bash", "read_file"}),
		makeToolResultOpenAI("call_a", "result_a"),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	foundB := false
	for _, m := range arr.Items {
		if m.GetString("tool_call_id") == "call_b" {
			foundB = true
			if c := m.GetString("content"); !strings.Contains(c, "cancelled") {
				t.Fatalf("call_b content = %q, want it to mention cancelled", c)
			}
		}
	}
	if !foundB {
		t.Fatal("synthetic result for call_b not found")
	}
}

// The synthetic result must land directly after the run of results already
// following the call, not at the end of the transcript: a provider reads the
// pairing positionally, and this is the one ordering rule the C pointer splice
// existed to honour.
func TestRepairInsertsAfterExistingResultsOpenAI(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "do stuff"),
		makeAssistantWithToolsOpenAI([]string{"call_a", "call_b"}, []string{"bash", "read_file"}),
		makeToolResultOpenAI("call_a", "result_a"),
		makeMsg("assistant", "all done"),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	if arr.Len() != 5 {
		t.Fatalf("len = %d, want 5", arr.Len())
	}
	// user, assistant(calls), tool(call_a), tool(call_b synthetic), assistant
	if id := arr.At(3).GetString("tool_call_id"); id != "call_b" {
		t.Fatalf("index 3 tool_call_id = %q, want call_b", id)
	}
	if role := arr.At(4).GetString("role"); role != "assistant" {
		t.Fatalf("index 4 role = %q, want assistant (trailing message must stay last)", role)
	}
}

func TestRepairOrphanedCallAnthropic(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "list files"),
		makeAssistantWithToolsAnthropic([]string{"toolu_orphan"}, []string{"bash"}),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	if arr.Len() != 3 {
		t.Fatalf("len = %d, want 3", arr.Len())
	}
	last := arr.At(2)
	if role := last.GetString("role"); role != "user" {
		t.Fatalf("role = %q, want user", role)
	}
	content := last.Get("content")
	if !content.IsArray() {
		t.Fatal("content is not an array")
	}
	tr := content.At(0)
	if typ := tr.GetString("type"); typ != "tool_result" {
		t.Fatalf("type = %q, want tool_result", typ)
	}
	if id := tr.GetString("tool_use_id"); id != "toolu_orphan" {
		t.Fatalf("tool_use_id = %q, want toolu_orphan", id)
	}
}

func TestRepairConsistentAnthropic(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "list files"),
		makeAssistantWithToolsAnthropic([]string{"toolu_1"}, []string{"bash"}),
		makeToolResultsAnthropic([]string{"toolu_1"}, []string{"file1.txt"}),
	)
	if got := MessageHistoryRepair(arr); got != 0 {
		t.Fatalf("got %d repairs, want 0", got)
	}
	if arr.Len() != 3 {
		t.Fatalf("len = %d, want 3", arr.Len())
	}
}

func TestRepairOrphanedResultAnthropic(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "hello"),
		makeAssistantWithToolsAnthropic([]string{"toolu_real"}, []string{"bash"}),
		makeToolResultsAnthropic([]string{"toolu_real", "toolu_ghost"}, []string{"ok", "orphan"}),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	content := arr.At(2).Get("content")
	if content.Len() != 1 {
		t.Fatalf("content len = %d, want 1", content.Len())
	}
	if id := content.At(0).GetString("tool_use_id"); id != "toolu_real" {
		t.Fatalf("surviving tool_use_id = %q, want toolu_real", id)
	}
}

// A user message left empty by removing its only (orphaned) tool_result is
// dropped, and that drop is NOT counted as a repair.
func TestRepairDropsEmptiedUserMessageAnthropic(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "hello"),
		makeAssistantWithToolsAnthropic([]string{"toolu_real"}, []string{"bash"}),
		makeToolResultsAnthropic([]string{"toolu_real"}, []string{"ok"}),
		makeToolResultsAnthropic([]string{"toolu_ghost"}, []string{"orphan"}),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1 (the block removal only)", got)
	}
	if arr.Len() != 3 {
		t.Fatalf("len = %d, want 3 (emptied user message dropped)", arr.Len())
	}
}

// An id that is PRESENT but empty is a real id that can never pair, not a
// missing field: the result is an orphan and must be removed. Found by
// differential testing against the C, which distinguishes the two via a NULL
// check where Go's GetString collapses both to "".
func TestRepairEmptyIDIsAnOrphanNotAnAbsentField(t *testing.T) {
	present := NewObject()
	present.Set("role", NewString("tool"))
	present.Set("tool_call_id", NewString(""))
	present.Set("content", NewString("x"))
	arr := arrayOf(makeMsg("user", "u"), present)

	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1 (empty tool_call_id is unmatchable)", got)
	}
	if arr.Len() != 1 {
		t.Fatalf("len = %d, want 1 (the orphan is removed)", arr.Len())
	}

	// With the field absent entirely there is nothing to pair on, so the message
	// is left alone -- and with no ids anywhere, repair short-circuits at 0.
	absent := NewObject()
	absent.Set("role", NewString("tool"))
	absent.Set("content", NewString("x"))
	arr2 := arrayOf(makeMsg("user", "u"), absent)
	if got := MessageHistoryRepair(arr2); got != 0 {
		t.Fatalf("absent field: got %d repairs, want 0", got)
	}
	if arr2.Len() != 2 {
		t.Fatalf("absent field: len = %d, want 2", arr2.Len())
	}
}

// An empty call id and an empty result id pair with each other, because "" is a
// legitimate set member rather than a sentinel.
func TestRepairEmptyIDsPairWithEachOther(t *testing.T) {
	assistant := makeAssistantWithToolsOpenAI([]string{""}, []string{"bash"})
	result := NewObject()
	result.Set("role", NewString("tool"))
	result.Set("tool_call_id", NewString(""))
	result.Set("content", NewString("ok"))
	arr := arrayOf(makeMsg("user", "u"), assistant, result)

	if got := MessageHistoryRepair(arr); got != 0 {
		t.Fatalf("got %d repairs, want 0 (empty ids pair)", got)
	}
	if arr.Len() != 3 {
		t.Fatalf("len = %d, want 3", arr.Len())
	}
}

// A duplicated call id yields ONE synthetic result, not one per occurrence.
// This is the deliberate divergence from the C, which emitted a result per
// occurrence and so produced two results for a single call -- a transcript
// providers reject. Pinned so the dedupe is not "fixed" back later.
func TestRepairDuplicateCallIDRepairedOnce(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "u"),
		makeAssistantWithToolsOpenAI([]string{"dup"}, []string{"bash"}),
		makeAssistantWithToolsOpenAI([]string{"dup"}, []string{"bash"}),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	results := 0
	for _, m := range arr.Items {
		if m.GetString("role") == "tool" && m.GetString("tool_call_id") == "dup" {
			results++
		}
	}
	if results != 1 {
		t.Fatalf("%d synthetic results for one call id, want 1", results)
	}
}

func TestRepairResponsesAPI(t *testing.T) {
	fc := NewObject()
	fc.Set("type", NewString("function_call"))
	fc.Set("call_id", NewString("fc_orphan"))
	fc.Set("name", NewString("bash"))
	fc.Set("arguments", NewString("{}"))
	arr := arrayOf(fc)

	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("got %d repairs, want 1", got)
	}
	if arr.Len() != 2 {
		t.Fatalf("len = %d, want 2", arr.Len())
	}
	out := arr.At(1)
	if typ := out.GetString("type"); typ != "function_call_output" {
		t.Fatalf("type = %q, want function_call_output", typ)
	}
	if id := out.GetString("call_id"); id != "fc_orphan" {
		t.Fatalf("call_id = %q, want fc_orphan", id)
	}
}

func TestRepairIdempotent(t *testing.T) {
	arr := arrayOf(
		makeMsg("user", "do stuff"),
		makeAssistantWithToolsOpenAI([]string{"call_x"}, []string{"bash"}),
	)
	if got := MessageHistoryRepair(arr); got != 1 {
		t.Fatalf("first pass: got %d, want 1", got)
	}
	sizeAfterFirst := arr.Len()
	if got := MessageHistoryRepair(arr); got != 0 {
		t.Fatalf("second pass: got %d, want 0", got)
	}
	if arr.Len() != sizeAfterFirst {
		t.Fatalf("len changed on second pass: %d -> %d", sizeAfterFirst, arr.Len())
	}
}

// Ported from src/tests/test_minimax_tool_call_args.c: strict providers reject
// the whole request when arguments is absent, an object, or unparseable.
func TestSanitizeAssistantToolArguments(t *testing.T) {
	build := func(args *JSONValue) *JSONValue {
		fn := NewObject()
		fn.Set("name", NewString("bash"))
		if args != nil {
			fn.Set("arguments", args)
		}
		tc := NewObject()
		tc.Set("id", NewString("call_1"))
		tc.Set("function", fn)
		tcs := NewArray()
		tcs.Append(tc)
		m := NewObject()
		m.Set("role", NewString("assistant"))
		m.Set("tool_calls", tcs)
		return m
	}
	argsOf := func(m *JSONValue) *JSONValue {
		return m.Get("tool_calls").At(0).Get("function").Get("arguments")
	}

	// A valid JSON string is left exactly as it was.
	valid := build(NewString(`{"cmd":"ls"}`))
	SanitizeAssistantToolArguments(valid)
	if got := argsOf(valid).Str; got != `{"cmd":"ls"}` {
		t.Fatalf("valid arguments rewritten to %q", got)
	}

	// Missing, object-valued, and unparseable all become "{}".
	for name, m := range map[string]*JSONValue{
		"missing":     build(nil),
		"object":      build(NewObject()),
		"unparseable": build(NewString("{not json")),
	} {
		SanitizeAssistantToolArguments(m)
		got := argsOf(m)
		if !got.IsString() || got.Str != "{}" {
			t.Fatalf("%s: arguments = %+v, want the string {}", name, got)
		}
	}
}

// The repair runs the sanitiser on every assistant message even when tool
// pairing is already consistent, because an unparseable arguments string is
// rejected independently of pairing.
func TestRepairSanitizesArgumentsOnConsistentHistory(t *testing.T) {
	assistant := makeAssistantWithToolsOpenAI([]string{"call_1"}, []string{"bash"})
	assistant.Get("tool_calls").At(0).Get("function").Set("arguments", NewString("{not json"))
	arr := arrayOf(
		makeMsg("user", "list files"),
		assistant,
		makeToolResultOpenAI("call_1", "ok"),
	)
	if got := MessageHistoryRepair(arr); got != 0 {
		t.Fatalf("got %d repairs, want 0 (pairing was consistent)", got)
	}
	args := arr.At(1).Get("tool_calls").At(0).Get("function").Get("arguments")
	if !args.IsString() || args.Str != "{}" {
		t.Fatalf("arguments = %+v, want the string {}", args)
	}
}
