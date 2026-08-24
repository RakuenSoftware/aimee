package families

import (
	"context"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// session builds a clarify session from pairs written as
// "dimension:answered", which is all the scoring and selection logic reads.
func session(description string, pairs ...string) clarifySession {
	s := clarifySession{description: description, status: "open"}
	for i, p := range pairs {
		dim, state, _ := strings.Cut(p, ":")
		var answered int64
		if state == "answered" {
			answered = 1
		}
		s.qa = append(s.qa, clarifyQA{
			dimension: dim,
			question:  "q" + dim,
			answer:    "a" + dim,
			answered:  answered,
			seq:       int64(i),
		})
	}
	return s
}

// The defect: the C's pending-question check ran in a loop starting at index 1,
// so dimension 0 was seeded as the answer and never checked. A session waiting
// on a scope answer was told to ask about scope again -- the same question, a
// second time, while the first was still outstanding.
func TestWeakestDimensionSkipsOneAlreadyWaitingOnAnAnswer(t *testing.T) {
	// scope has an unanswered question outstanding. Every other dimension is
	// untouched and therefore also scores zero, so the only thing that can keep
	// the answer off scope is the pending check applying to it.
	s := session("a task", "scope:pending")

	if got := clarifyWeakestDim(s); got == "scope" {
		t.Fatalf("chose scope, which already has an unanswered question outstanding")
	}

	// And the pending check must not be so broad that it skips a dimension
	// whose only pair is already answered.
	s = session("a task", "scope:answered")
	if got := clarifyWeakestDim(s); got == "scope" {
		t.Fatalf("chose scope, the only dimension with any coverage at all: %s", got)
	}
}

// With every dimension outstanding there is no unblocked one to name. The C
// always returned something and the caller's pair limit is what ends the loop,
// so this must still answer rather than fail.
func TestWeakestDimensionStillAnswersWhenEveryDimensionIsPending(t *testing.T) {
	var pairs []string
	for _, d := range clarifyDims {
		pairs = append(pairs, d.name+":pending")
	}
	if got := clarifyWeakestDim(session("a task", pairs...)); got == "" {
		t.Fatalf("named no dimension at all")
	}
}

// The least-covered dimension wins: one answer scores below none.
func TestWeakestDimensionPrefersTheLeastCovered(t *testing.T) {
	// Every dimension but "context" has an answer, so context is the weakest.
	var pairs []string
	for _, d := range clarifyDims {
		if d.name != "context" {
			pairs = append(pairs, d.name+":answered")
		}
	}
	if got := clarifyWeakestDim(session("a task", pairs...)); got != "context" {
		t.Fatalf("chose %q, want the one dimension with no answer", got)
	}
}

// The defect: the C's helper returns 0 when it PRODUCED a question and 1 when
// none is needed, and the dispatch mapped positive to OK and zero to MISSING.
// So the caller was told MISSING exactly when there was a question to ask, and
// OK -- with two empty cells -- when there was not. A caller that trusted the
// status would never ask a clarifying question.
func TestNextQuestionReportsOKWhenThereIsAQuestion(t *testing.T) {
	s := session("a short task")
	fields := encodeClarifySession(s)

	status, reply, err := clarifyNextQuestionOp(context.Background(), nil, fields)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if status != store.StatusOK {
		t.Fatalf("status %d for a session that has a question to ask, want OK", status)
	}
	if len(reply) != 2 || reply[0] == "" || reply[1] == "" {
		t.Fatalf("reported OK but answered with %q", reply)
	}
	if clarifyDimIndex(reply[1]) < 0 {
		t.Fatalf("named %q, which is not a dimension", reply[1])
	}
}

// The other half: a session with nothing left to ask reports MISSING, not an OK
// carrying two empty strings that the caller would render as a question.
func TestNextQuestionReportsMissingWhenThereIsNone(t *testing.T) {
	s := session("a task")
	s.status = "ready"

	status, reply, err := clarifyNextQuestionOp(context.Background(), nil, encodeClarifySession(s))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if status != store.StatusMissing {
		t.Fatalf("status %d for a ready session with no question, want MISSING", status)
	}
	for _, cell := range reply {
		if cell != "" {
			t.Fatalf("answered MISSING but carried %q", reply)
		}
	}
}

// A session that has used up its pairs has no next question either, however low
// it scores -- otherwise the caller loops forever against a full array.
func TestNextQuestionStopsAtThePairLimit(t *testing.T) {
	var pairs []string
	for i := 0; i < clarifyMaxQA; i++ {
		pairs = append(pairs, "scope:pending")
	}
	s := session("a short task", pairs...)
	if _, _, ok := clarifyNextQuestion(s); ok {
		t.Fatalf("offered a ninth question for an eight-pair session")
	}
}

// Scoring: the base comes from the description length, and coverage is the
// average across all five dimensions.
func TestScoreCombinesDescriptionLengthAndCoverage(t *testing.T) {
	short := clarifyScore(session("tiny"))
	long := clarifyScore(session(strings.Repeat("x", 200)))
	if !(long > short) {
		t.Fatalf("a 200-character description scored %v, no better than a 4-character one at %v", long, short)
	}

	// One answer in one dimension is worth 0.7/5 of the 0.70 coverage share.
	none := clarifyScore(session("tiny"))
	one := clarifyScore(session("tiny", "scope:answered"))
	if !(one > none) {
		t.Fatalf("an answered question did not raise the score: %v then %v", none, one)
	}
	// A second answer in the SAME dimension takes it from 0.7 to 1.0; a third
	// adds nothing, because the dimension is already fully covered.
	two := clarifyScore(session("tiny", "scope:answered", "scope:answered"))
	three := clarifyScore(session("tiny", "scope:answered", "scope:answered", "scope:answered"))
	if !(two > one) {
		t.Fatalf("a second answer in a dimension did not raise the score: %v then %v", one, two)
	}
	if three != two {
		t.Fatalf("a third answer in an already-covered dimension changed the score: %v then %v", two, three)
	}
}

// An unanswered pair contributes nothing. Asking a question must not be worth
// the same as having it answered.
func TestScoreIgnoresUnansweredPairs(t *testing.T) {
	asked := clarifyScore(session("tiny", "scope:pending"))
	silent := clarifyScore(session("tiny"))
	if asked != silent {
		t.Fatalf("asking a question scored %v against %v for asking nothing", asked, silent)
	}
}

// The score is capped, so a fully covered long description cannot exceed 1.
func TestScoreIsCapped(t *testing.T) {
	var pairs []string
	for _, d := range clarifyDims {
		pairs = append(pairs, d.name+":answered", d.name+":answered")
	}
	if got := clarifyScore(session(strings.Repeat("x", 400), pairs...)); got > 1 {
		t.Fatalf("scored %v, above the cap", got)
	}
}

// Crystallizing renders the answered pairs and omits the outstanding ones --
// an unanswered question in a specification reads as a requirement.
func TestCrystallizeRendersOnlyAnsweredPairs(t *testing.T) {
	s := session("build the thing", "scope:answered", "constraints:pending")
	spec := clarifyCrystallize(s)

	if !strings.Contains(spec, "build the thing") {
		t.Fatalf("the specification omits the task: %s", spec)
	}
	if !strings.Contains(spec, "ascope") {
		t.Fatalf("the specification omits an answer it has: %s", spec)
	}
	if strings.Contains(spec, "qconstraints") {
		t.Fatalf("the specification carries a question nobody answered: %s", spec)
	}
}

// The 48-field session round-trips: what the store replies with is what the
// pure operations accept, so a caller never has to reshape one.
func TestSessionRoundTripsThroughTheWire(t *testing.T) {
	s := session("a task", "scope:answered", "approach:pending")
	s.id, s.spec, s.createdAt, s.updatedAt = 42, "spec text", "2026-01-01 00:00:00", "2026-01-02 00:00:00"

	fields := encodeClarifySession(s)
	if len(fields) != clarifySessFields {
		t.Fatalf("encoded %d fields, the wire carries %d", len(fields), clarifySessFields)
	}
	back, ok := decodeClarifySession(fields)
	if !ok {
		t.Fatalf("the store's own reply did not decode")
	}
	if back.id != s.id || back.description != s.description || back.spec != s.spec {
		t.Fatalf("round trip changed the session: %+v against %+v", back, s)
	}
	if len(back.qa) != len(s.qa) {
		t.Fatalf("round trip carried %d pairs, sent %d", len(back.qa), len(s.qa))
	}
	for i := range s.qa {
		if back.qa[i] != s.qa[i] {
			t.Fatalf("pair %d changed: %+v against %+v", i, back.qa[i], s.qa[i])
		}
	}
}

// A frame of the wrong width is refused rather than indexed into.
func TestSessionDecodeRefusesTheWrongWidth(t *testing.T) {
	for _, fields := range [][]string{
		nil,
		make([]string, clarifySessFields-1),
		make([]string, clarifySessFields+1),
	} {
		if _, ok := decodeClarifySession(fields); ok {
			t.Fatalf("accepted a %d-field session, the wire carries %d",
				len(fields), clarifySessFields)
		}
	}
}

// A pair count past the array is refused rather than clamped: clamping would
// score the session against fewer pairs than the caller believes it sent, and
// answer as though that were the whole session.
func TestSessionDecodeRefusesAPairCountPastTheArray(t *testing.T) {
	fields := encodeClarifySession(session("a task"))
	countAt := clarifyQAOffset + clarifyMaxQA*clarifyQAWidth
	for _, count := range []string{"9", "-1", "1000", "not a number"} {
		fields[countAt] = count
		if _, ok := decodeClarifySession(fields); ok {
			t.Fatalf("accepted a pair count of %q", count)
		}
	}
}
