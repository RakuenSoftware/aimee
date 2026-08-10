package delegates

import (
	"encoding/json"
	"os"
	"strings"
	"testing"
)

// The golden corpus was generated from the C parser this package replaces, and
// is now owned here: the generator went with the rule, because regenerating
// from a C side that no longer implements the dialects would have emptied it.
// Pinning
// the Go port against it is what makes this a port rather than a rewrite: every
// dialect, every refusal and every argument spelling has to agree byte for byte.

type goldenCall struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Arguments string `json:"arguments"`
}

type goldenCase struct {
	Name         string       `json:"name"`
	Text         string       `json:"text"`
	AllowJSON    bool         `json:"allow_json"`
	RC           int          `json:"rc"`
	IsToolCall   bool         `json:"is_tool_call"`
	Content      *string      `json:"content"`
	Detected     bool         `json:"detected"`
	DetectedJSON bool         `json:"detected_json"`
	Calls        []goldenCall `json:"calls"`
}

type goldenFile struct {
	KnownTools []string     `json:"known_tools"`
	Cases      []goldenCase `json:"cases"`
}

func loadGolden(t *testing.T) goldenFile {
	t.Helper()
	raw, err := os.ReadFile("testdata/xml_fallback_golden.json")
	if err != nil {
		t.Fatalf("read golden: %v", err)
	}
	var g goldenFile
	if err := json.Unmarshal(raw, &g); err != nil {
		t.Fatalf("parse golden: %v", err)
	}
	if len(g.Cases) == 0 {
		t.Fatal("golden corpus is empty")
	}
	return g
}

func TestRescueMatchesGoldenCorpus(t *testing.T) {
	g := loadGolden(t)
	for _, c := range g.Cases {
		t.Run(c.Name, func(t *testing.T) {
			got, rc := RescueParseToolCalls(c.Text, g.KnownTools, c.AllowJSON)
			if rc != c.RC {
				t.Errorf("rc = %d, want %d", rc, c.RC)
			}
			if got.IsToolCall != c.IsToolCall {
				t.Errorf("is_tool_call = %v, want %v", got.IsToolCall, c.IsToolCall)
			}
			wantContent := ""
			if c.Content != nil {
				wantContent = *c.Content
			}
			if got.Content != wantContent {
				t.Errorf("content = %q, want %q", got.Content, wantContent)
			}
			if len(got.Calls) != len(c.Calls) {
				t.Fatalf("call count = %d, want %d (%+v)", len(got.Calls), len(c.Calls), got.Calls)
			}
			for i, want := range c.Calls {
				have := got.Calls[i]
				if have.ID != want.ID || have.Name != want.Name || have.Arguments != want.Arguments {
					t.Errorf("call %d = {%s %s %s}, want {%s %s %s}", i,
						have.ID, have.Name, have.Arguments,
						want.ID, want.Name, want.Arguments)
				}
			}
		})
	}
}

// Ported from test_agent_http.c. Both are shapes the golden corpus does not
// reach, so they would have been lost with the C tests.

// A nameless <invoke> is not a call, and must not stop a real one that follows
// it from being found.
func TestRescueSkipsMalformedInvokeAndKeepsTheValidOne(t *testing.T) {
	text := "<invoke>\n<parameter name=\"command\">echo ignored</parameter>\n</invoke>\n" +
		"<invoke name=\"bash\">\n<parameter name=\"command\">echo parsed</parameter>\n</invoke>"
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "bash" || !strings.Contains(got.Calls[0].Arguments, "parsed") {
		t.Errorf("call = %+v", got.Calls[0])
	}
}

// A channel payload with an unquoted key is not an object, so it takes the
// key:value path -- and the brace inside the quoted value must not end the
// arguments early.
func TestRescueChannelBalancesBracesInsideAString(t *testing.T) {
	text := `<|channel>call: bash {command: "echo } ok"}<tool_call|>`
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "bash" || got.Calls[0].Arguments != `{"command":"echo } ok"}` {
		t.Errorf("call = %+v", got.Calls[0])
	}
}

// Ported from test_delegate_driver.c.

// "respond" is the synthetic tool a delegate answers through, so it is always
// dispatchable whether or not it appears in the caller's inventory.
func TestRescueAlwaysAcceptsRespond(t *testing.T) {
	text := `{"name":"respond","arguments":{"message":"done"}}`
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "respond" || got.Calls[0].Arguments != `{"message":"done"}` {
		t.Errorf("call = %+v", got.Calls[0])
	}
}

// The tool/args spelling, announced in prose: the prose stays as content.
func TestRescueReadsToolArgsSpellingAfterProse(t *testing.T) {
	text := `Next I will run {"tool":"bash","args":{"command":"pwd"}}`
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "bash" || got.Calls[0].Arguments != `{"command":"pwd"}` {
		t.Errorf("call = %+v", got.Calls[0])
	}
	if !strings.Contains(got.Content, "Next I will run") {
		t.Errorf("content = %q", got.Content)
	}
}

// A block written across lines with indented tags: the name and arguments are
// trimmed, and the prose before it survives as content.
func TestRescueReadsAMultilineBlock(t *testing.T) {
	text := "I will run the command now.\n<tool_call>\n  <name>bash</name>\n" +
		"  <arguments>{\"command\": \"ls -la\"}</arguments>\n</tool_call>\n"
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "bash" || !strings.Contains(got.Calls[0].Arguments, "ls -la") {
		t.Errorf("call = %+v", got.Calls[0])
	}
	if !strings.Contains(got.Content, "run the command") {
		t.Errorf("content = %q", got.Content)
	}
}

// harmony encodes quotes as <|"|>. Decoding them is what makes the payload an
// object at all, and the decoded quotes must not end the arguments early.
func TestRescueChannelDecodesQuoteMarkers(t *testing.T) {
	text := `<|channel>call:bash{command:<|"|>git show 0409c38e<|"|>}<tool_call|>`
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Arguments != `{"command":"git show 0409c38e"}` {
		t.Errorf("arguments = %q", got.Calls[0].Arguments)
	}
}

// A Qwen parameter that looks like a number becomes one, so the executor gets
// offset 0 rather than the string "0".
func TestRescueQwenParameterTypesSurvive(t *testing.T) {
	text := "I need to read the file.\n<tool_call>\n<function=read_file>\n" +
		"<parameter=path>/tmp/probe.txt</parameter>\n<parameter=offset>0</parameter>\n" +
		"</function>\n</tool_call>"
	got, rc := RescueParseToolCalls(text, []string{"read_file"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "read_file" || got.Calls[0].ID != "xml_call_1" {
		t.Errorf("call = %+v", got.Calls[0])
	}
	if got.Calls[0].Arguments != `{"path":"/tmp/probe.txt","offset":0}` {
		t.Errorf("arguments = %q", got.Calls[0].Arguments)
	}
}

// Reasoning is stripped before the bracket dialect is read, so the think block
// neither hides the call nor leaks into the content.
func TestRescueMistralAfterAThinkBlock(t *testing.T) {
	text := "<think>I should inspect the file first.</think>\n" +
		`[TOOL_CALLS]read_file{"path":"src/main.c","limit":20,"meta":{"nested":true}}`
	got, rc := RescueParseToolCalls(text, []string{"read_file"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if !strings.Contains(got.Calls[0].Arguments, `"path":"src/main.c"`) ||
		!strings.Contains(got.Calls[0].Arguments, `"nested":true`) {
		t.Errorf("arguments = %q", got.Calls[0].Arguments)
	}
	if strings.Contains(got.Content, "think") {
		t.Errorf("reasoning leaked into content: %q", got.Content)
	}
}

// The minimax shape: a namespaced block whose body is an <invoke>. Neither the
// plain nor the namespaced <tool_call> scan finds a name here, so this is what
// the local-name fallback pass exists for -- and "Bash" folds to the registered
// spelling on the way out.
func TestRescueReadsANamespacedInvokeBlock(t *testing.T) {
	text := "<minimax:tool_call>\n<invoke name=\"Bash\">\n" +
		"<parameter name=\"description\">Check current branch</parameter>\n" +
		"<parameter name=\"command\">git branch --show-current</parameter>\n" +
		"</invoke>\n</minimax:tool_call>"
	got, rc := RescueParseToolCalls(text, []string{"bash"}, true)
	if rc != 1 || len(got.Calls) != 1 {
		t.Fatalf("rc = %d, calls = %+v", rc, got.Calls)
	}
	if got.Calls[0].Name != "bash" {
		t.Errorf("name = %q, want bash", got.Calls[0].Name)
	}
	if !strings.Contains(got.Calls[0].Arguments, `"command":"git branch --show-current"`) ||
		!strings.Contains(got.Calls[0].Arguments, `"description":"Check current branch"`) {
		t.Errorf("arguments = %q", got.Calls[0].Arguments)
	}
}

func TestRescueDetectionMatchesGoldenCorpus(t *testing.T) {
	g := loadGolden(t)
	for _, c := range g.Cases {
		t.Run(c.Name, func(t *testing.T) {
			// "detected" is the always-allow-JSON detector; "detected_json"
			// applies the case's own allow_json, which is how the caller asks
			// whether prose JSON counts.
			if got := RescueHasToolCalls(c.Text, g.KnownTools, true); got != c.Detected {
				t.Errorf("detect(allow_json=true) = %v, want %v", got, c.Detected)
			}
			if got := RescueHasToolCalls(c.Text, g.KnownTools, c.AllowJSON); got != c.DetectedJSON {
				t.Errorf("detect(allow_json=%v) = %v, want %v", c.AllowJSON, got, c.DetectedJSON)
			}
		})
	}
}
