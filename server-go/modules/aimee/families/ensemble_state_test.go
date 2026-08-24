package families

import (
	"path/filepath"
	"strings"
	"testing"
)

// A template name is a bare filename, and the check happens BEFORE it is
// joined to anything.
//
// The C checked only that the name was non-empty and interpolated it into
// "%s/%s/%s.json", so a name containing ../ escaped the templates directory:
// the store read any .json file the daemon could reach, stored the contents in
// template_json, and handed them back on the next view. That is an arbitrary
// file read reachable by anyone who can ask to start an ensemble in a channel.
func TestTemplateNameRefusesAnythingThatIsNotABareFilename(t *testing.T) {
	for _, name := range []string{
		"../secrets",
		"../../etc/hosts",
		"a/../../b",
		"nested/name",
		"/absolute",
		"..",
		"...",
		"a..b",
		"",
		".hidden",
		"-leading-dash-is-fine-but-not-a-dot",
		strings.Repeat("x", 65),
		"name with spaces",
		"name;with;semicolons",
		"name\x00truncated",
	} {
		if validTemplateName(name) {
			t.Errorf("accepted %q as a template name", name)
		}
	}
}

func TestTemplateNameAcceptsOrdinaryNames(t *testing.T) {
	for _, name := range []string{
		"code-review", "debate", "planning", "design-critique",
		"my_template", "v2.plan", "a", "A1",
	} {
		if !validTemplateName(name) {
			t.Errorf("refused %q, which is an ordinary template name", name)
		}
	}
}

// The traversal must be impossible even if the name check were somehow passed:
// every candidate path stays under one of the two roots.
func TestTemplateCandidatesStayUnderTheirRoots(t *testing.T) {
	roots := []string{"/srv/project", "/etc/aimee"}
	for _, name := range []string{"code-review", "my_template", "v2.plan"} {
		for _, path := range templateCandidates(roots[0], roots[1], name) {
			clean := filepath.Clean(path)
			under := false
			for _, root := range roots {
				if strings.HasPrefix(clean, root+string(filepath.Separator)) {
					under = true
				}
			}
			if !under {
				t.Errorf("candidate %q for %q is not under either root", clean, name)
			}
		}
	}
}

// --- the turn ---------------------------------------------------------------

// twoPhase is a run with two turns in the first phase and one in the second,
// so it exercises the turn boundary, the phase boundary and completion.
func twoPhase() template {
	return template{
		Name: "test",
		Phases: []phase{
			{Name: "review", Participants: []participant{
				{Agent: "alice", Role: "reviewer"},
				{Agent: "bob", Role: "reviewer"},
			}},
			{Name: "verdict", Participants: []participant{
				{Agent: "carol", Role: "author"},
			}},
		},
	}
}

func start(t template) ensembleState {
	s := ensembleState{Status: "active"}
	s.resolveExpectation(t)
	return s
}

func TestATurnMovesToTheNextParticipant(t *testing.T) {
	tmpl := twoPhase()
	s := start(tmpl)
	if s.ExpectedAgent != "alice" {
		t.Fatalf("the run opens expecting %q, want alice", s.ExpectedAgent)
	}

	s, ctx, outcome := advance(tmpl, s, nil, "alice", "my review")
	if outcome != advanceTook {
		t.Fatalf("the expected agent's turn was not taken (outcome %d)", outcome)
	}
	if s.CurrentPhase != 0 || s.CurrentTurn != 1 {
		t.Fatalf("moved to phase %d turn %d, want phase 0 turn 1", s.CurrentPhase, s.CurrentTurn)
	}
	if s.ExpectedAgent != "bob" {
		t.Fatalf("now expecting %q, want bob", s.ExpectedAgent)
	}
	if len(ctx) != 1 || ctx[0].Sender != "alice" || ctx[0].Text != "my review" {
		t.Fatalf("the transcript did not record the turn: %+v", ctx)
	}
	// The message is filed under the turn it was spoken at, not the next one.
	if ctx[0].Phase != 0 || ctx[0].Turn != 0 {
		t.Fatalf("the message is filed at phase %d turn %d, want where it was spoken",
			ctx[0].Phase, ctx[0].Turn)
	}
}

func TestTheLastTurnOfAPhaseMovesToTheNextPhase(t *testing.T) {
	tmpl := twoPhase()
	s := start(tmpl)
	s, ctx, _ := advance(tmpl, s, nil, "alice", "a")
	s, _, outcome := advance(tmpl, s, ctx, "bob", "b")

	if outcome != advanceTook {
		t.Fatalf("bob's turn was not taken")
	}
	if s.CurrentPhase != 1 || s.CurrentTurn != 0 {
		t.Fatalf("moved to phase %d turn %d, want phase 1 turn 0", s.CurrentPhase, s.CurrentTurn)
	}
	if s.ExpectedAgent != "carol" {
		t.Fatalf("now expecting %q, want carol", s.ExpectedAgent)
	}
}

func TestTheLastTurnOfTheLastPhaseCompletesTheRun(t *testing.T) {
	tmpl := twoPhase()
	s := start(tmpl)
	s, ctx, _ := advance(tmpl, s, nil, "alice", "a")
	s, ctx, _ = advance(tmpl, s, ctx, "bob", "b")
	s, ctx, outcome := advance(tmpl, s, ctx, "carol", "c")

	if outcome != advanceTook {
		t.Fatalf("carol's turn was not taken")
	}
	if s.Status != "complete" {
		t.Fatalf("status %q after the final turn, want complete", s.Status)
	}
	if s.ExpectedAgent != "" || s.ExpectedRole != "" {
		t.Fatalf("a complete run still expects %q/%q", s.ExpectedAgent, s.ExpectedRole)
	}
	// It parks on the last phase rather than one past it, so a reader sees
	// where the run ended rather than an index into nothing.
	if s.CurrentPhase != len(tmpl.Phases)-1 {
		t.Fatalf("a complete run sits on phase %d, want the last one (%d)",
			s.CurrentPhase, len(tmpl.Phases)-1)
	}
	if len(ctx) != 3 {
		t.Fatalf("the transcript holds %d messages, want 3", len(ctx))
	}
}

// An agent in the cast speaking out of turn is a caller mistake: the run does
// not move and nothing is recorded.
func TestAnAgentSpeakingOutOfTurnIsRefused(t *testing.T) {
	tmpl := twoPhase()
	s := start(tmpl)

	after, ctx, outcome := advance(tmpl, s, nil, "bob", "jumping in")
	if outcome != advanceRefused {
		t.Fatalf("bob spoke out of turn and was not refused (outcome %d)", outcome)
	}
	if after != s {
		t.Fatalf("a refused turn changed the run: %+v against %+v", after, s)
	}
	if len(ctx) != 0 {
		t.Fatalf("a refused turn recorded %d messages", len(ctx))
	}
}

// Someone outside the cast is a person talking in the channel. That pauses the
// run rather than being rejected -- and their message IS recorded, because it
// is the reason the run stopped.
func TestSomeoneOutsideTheCastPausesTheRun(t *testing.T) {
	tmpl := twoPhase()
	s := start(tmpl)

	s, ctx, outcome := advance(tmpl, s, nil, "a-person", "hold on")
	if outcome != advanceInterrupted {
		t.Fatalf("an outsider did not interrupt the run (outcome %d)", outcome)
	}
	if s.Status != "paused" {
		t.Fatalf("status %q after an interruption, want paused", s.Status)
	}
	if s.PausedReason != "human_interruption" {
		t.Fatalf("paused for %q, want human_interruption", s.PausedReason)
	}
	if len(ctx) != 1 || ctx[0].Sender != "a-person" {
		t.Fatalf("the interruption was not recorded: %+v", ctx)
	}
	// The turn does not move: the expected agent still owes the run a turn.
	if s.CurrentTurn != 0 || s.ExpectedAgent != "alice" {
		t.Fatalf("an interruption consumed alice's turn: turn %d expecting %q",
			s.CurrentTurn, s.ExpectedAgent)
	}
}

// --- assignments -------------------------------------------------------------

// A role held twice in one phase goes to two different agents when the caller
// supplied two, and wraps when they supplied fewer.
func TestAssignmentsFillRepeatedRolesRoundRobin(t *testing.T) {
	tmpl := twoPhase()
	for i := range tmpl.Phases {
		for j := range tmpl.Phases[i].Participants {
			tmpl.Phases[i].Participants[j].Agent = ""
		}
	}

	err := expandAssignments(&tmpl, map[string][]string{
		"reviewer": {"alice", "bob"},
		"author":   {"carol"},
	})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	got := []string{
		tmpl.Phases[0].Participants[0].Agent,
		tmpl.Phases[0].Participants[1].Agent,
		tmpl.Phases[1].Participants[0].Agent,
	}
	want := []string{"alice", "bob", "carol"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("assignments came out %v, want %v", got, want)
		}
	}
}

func TestAssignmentsWrapWhenTooFewAgentsAreGiven(t *testing.T) {
	tmpl := twoPhase()
	if err := expandAssignments(&tmpl, map[string][]string{
		"reviewer": {"solo"},
		"author":   {"carol"},
	}); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if a, b := tmpl.Phases[0].Participants[0].Agent, tmpl.Phases[0].Participants[1].Agent; a != b {
		t.Fatalf("one agent for two reviewer turns gave %q and %q", a, b)
	}
}

// A role with no assignment names the role, because that is something the
// caller can act on.
func TestAMissingAssignmentNamesTheRole(t *testing.T) {
	tmpl := twoPhase()
	err := expandAssignments(&tmpl, map[string][]string{"reviewer": {"alice"}})
	if err == nil {
		t.Fatalf("a run started with no agent for the author role")
	}
	if !strings.Contains(err.Error(), "author") {
		t.Fatalf("the error does not name the unassigned role: %v", err)
	}
}

// --- prompts ------------------------------------------------------------------

func TestThePromptNamesThePhaseAgentAndRole(t *testing.T) {
	tmpl := twoPhase()
	prompt := buildPrompt(tmpl, nil, 0, 0)
	for _, want := range []string{"review", "alice", "reviewer"} {
		if !strings.Contains(prompt, want) {
			t.Errorf("the prompt omits %q:\n%s", want, prompt)
		}
	}
}

// A reviewer that echoes the previous reviewer has not reviewed anything, so
// the dissenting roles are told so.
func TestDissentingRolesAreToldToDisagree(t *testing.T) {
	tmpl := twoPhase()
	if !strings.Contains(buildPrompt(tmpl, nil, 0, 0), "independent analysis") {
		t.Errorf("the reviewer prompt carries no dissent instruction")
	}
	if strings.Contains(buildPrompt(tmpl, nil, 1, 0), "independent analysis") {
		t.Errorf("the author prompt carries a dissent instruction it should not")
	}
}

// The prompt carries only the tail of the conversation, so it does not grow
// without bound as a run goes on.
func TestThePromptCarriesOnlyTheRecentContext(t *testing.T) {
	tmpl := twoPhase()
	var ctx []contextMessage
	for _, name := range []string{"first", "second", "third", "fourth", "fifth", "sixth"} {
		ctx = append(ctx, contextMessage{Sender: name, Text: name + " said this"})
	}
	prompt := buildPrompt(tmpl, ctx, 0, 0)

	if strings.Contains(prompt, "first said this") {
		t.Errorf("the prompt carries the whole conversation:\n%s", prompt)
	}
	if !strings.Contains(prompt, "sixth said this") {
		t.Errorf("the prompt omits the most recent message:\n%s", prompt)
	}
	if got := strings.Count(prompt, " said this"); got != contextExcerptTurns {
		t.Errorf("the prompt carries %d messages, want %d", got, contextExcerptTurns)
	}
}

// --- reading a stored run ------------------------------------------------------

// A run whose position points past its template resolves to expecting nobody
// rather than indexing into nothing. A template edited under a live run makes
// this reachable.
func TestAPositionPastTheTemplateExpectsNobody(t *testing.T) {
	tmpl := twoPhase()
	for _, s := range []ensembleState{
		{Status: "active", CurrentPhase: 99, CurrentTurn: 0},
		{Status: "active", CurrentPhase: 0, CurrentTurn: 99},
		{Status: "active", CurrentPhase: -1, CurrentTurn: 0},
	} {
		s.resolveExpectation(tmpl)
		if s.ExpectedAgent != "" || s.ExpectedRole != "" {
			t.Errorf("phase %d turn %d resolved to %q/%q, want nobody",
				s.CurrentPhase, s.CurrentTurn, s.ExpectedAgent, s.ExpectedRole)
		}
	}
}

// An empty transcript stores as "[]" rather than "null": that is what the
// column's default says and what every reader of it expects.
func TestAnEmptyTranscriptEncodesAsAnEmptyArray(t *testing.T) {
	got, err := encodeContext(nil)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != "[]" {
		t.Fatalf("an empty transcript encoded as %q, want []", got)
	}
	back, err := parseContext(got)
	if err != nil || len(back) != 0 {
		t.Fatalf("the empty transcript did not round-trip: %v %v", back, err)
	}
}

// Every built-in template must parse and have phases, or a run started on one
// completes immediately with nobody having spoken.
func TestEveryBuiltinTemplateIsUsable(t *testing.T) {
	for name, raw := range builtinTemplates {
		tmpl, err := parseTemplate(raw)
		if err != nil {
			t.Errorf("built-in %q does not parse: %v", name, err)
			continue
		}
		if len(tmpl.Phases) == 0 {
			t.Errorf("built-in %q has no phases", name)
		}
		for i, ph := range tmpl.Phases {
			if len(ph.Participants) == 0 {
				t.Errorf("built-in %q phase %d has no participants", name, i)
			}
			for j, p := range ph.Participants {
				if p.Role == "" {
					t.Errorf("built-in %q phase %d participant %d has no role", name, i, j)
				}
			}
		}
	}
}
