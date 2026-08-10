package memory

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

// Each test starts from a closed breaker: the state is process-local, so a test
// that inherits an open breaker from the one before it passes or fails for
// reasons that have nothing to do with what it asserts.
func resetBreaker(t *testing.T) {
	t.Helper()
	breaker = embedBreaker{}
	t.Cleanup(func() { breaker = embedBreaker{} })
}

type embedStub struct {
	path, body, contentType string
	status                  int
	reply                   string
	calls                   int
}

func (s *embedStub) start(t *testing.T) string {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.calls++
		s.path = r.URL.RequestURI()
		s.contentType = r.Header.Get("Content-Type")
		buf := make([]byte, r.ContentLength)
		if r.ContentLength > 0 {
			_, _ = r.Body.Read(buf)
		}
		s.body = string(buf)
		if s.status == 0 {
			s.status = 200
		}
		w.WriteHeader(s.status)
		_, _ = w.Write([]byte(s.reply))
	}))
	t.Cleanup(server.Close)
	return server.URL
}

func TestEmbedPostsRawTextWithThePolarityInTheQuery(t *testing.T) {
	resetBreaker(t)
	stub := &embedStub{reply: `[0.5,-0.25,1]`}
	base := stub.start(t)

	out := Embed(EmbedRequest{BaseURL: base, InputType: "query", Text: "hello", MaxDim: 8, NowMS: 1000})
	if out.Error != "" {
		t.Fatalf("unexpected error: %s", out.Error)
	}
	// The body IS the raw text, which is why the polarity has to ride in the
	// query string; embedding a query as a document silently degrades recall.
	if stub.body != "hello" {
		t.Fatalf("body = %q, want the raw text", stub.body)
	}
	if !strings.Contains(stub.path, "/embed") || !strings.Contains(stub.path, "input_type=query") {
		t.Fatalf("path = %q", stub.path)
	}
	if out.Dim != 3 || out.Vector[0] != 0.5 || out.Vector[1] != -0.25 {
		t.Fatalf("vector = %+v", out)
	}
}

// Truncation must be REPORTED. Keeping the first max_dim leaves stored and query
// vectors inconsistent with the model's real output and quietly degrades recall,
// so silence here is the failure.
func TestEmbedReportsTruncationRatherThanHidingIt(t *testing.T) {
	resetBreaker(t)
	stub := &embedStub{reply: `[1,2,3,4,5]`}
	base := stub.start(t)

	out := Embed(EmbedRequest{BaseURL: base, Text: "t", MaxDim: 3, NowMS: 1000})
	if !out.Truncated {
		t.Fatal("over-long output must be reported as truncated")
	}
	if out.Dim != 3 {
		t.Fatalf("dim = %d, want the cap", out.Dim)
	}

	// At or under the cap there is nothing to report.
	resetBreaker(t)
	stub2 := &embedStub{reply: `[1,2,3]`}
	out = Embed(EmbedRequest{BaseURL: stub2.start(t), Text: "t", MaxDim: 3, NowMS: 1000})
	if out.Truncated || out.Dim != 3 {
		t.Fatalf("exactly at the cap is not truncation: %+v", out)
	}
}

// 401/403 proves the service is REACHABLE. Counting it as a failure would open
// the breaker against a service that answered, and a half-open breaker would
// then turn the next authorization result back into "unavailable".
func TestUnauthorizedIsReachabilityNotFailure(t *testing.T) {
	resetBreaker(t)
	// Put the breaker most of the way to open first, so a miscount would show.
	breaker.reportFailure(1000)
	breaker.reportFailure(1000)

	stub := &embedStub{status: 401, reply: `nope`}
	out := Embed(EmbedRequest{BaseURL: stub.start(t), Text: "t", MaxDim: 4, NowMS: 2000})

	if !out.Unauthorized || out.Error == "" {
		t.Fatalf("out = %+v", out)
	}
	if out.Unavailable {
		t.Fatal("unauthorized is not unavailable: the service answered")
	}
	// It CLOSES the earlier outage rather than adding to it.
	if breaker.failureStreak != 0 || breaker.retryAtMS != 0 {
		t.Fatalf("breaker must be closed after a reachable refusal: streak=%d retryAt=%d",
			breaker.failureStreak, breaker.retryAtMS)
	}
}

func TestBreakerOpensAfterThreeFailuresAndSuppressesWithoutCalling(t *testing.T) {
	resetBreaker(t)
	stub := &embedStub{status: 500, reply: ``}
	base := stub.start(t)

	for i := 0; i < breakerThreshold; i++ {
		out := Embed(EmbedRequest{BaseURL: base, Text: "t", MaxDim: 4, NowMS: 1000})
		if out.Error == "" {
			t.Fatalf("attempt %d should have failed", i)
		}
		if out.Unavailable {
			t.Fatalf("attempt %d was suppressed too early", i)
		}
	}
	callsBefore := stub.calls

	// Now suppressed: the point of the breaker is that NOTHING is sent.
	out := Embed(EmbedRequest{BaseURL: base, Text: "t", MaxDim: 4, NowMS: 1001})
	if !out.Unavailable {
		t.Fatalf("the breaker should be open: %+v", out)
	}
	if out.RetryAfterMS <= 0 {
		t.Fatalf("a suppressed call must say when to retry, got %d", out.RetryAfterMS)
	}
	if stub.calls != callsBefore {
		t.Fatalf("a suppressed call must not reach the service (%d -> %d)", callsBefore, stub.calls)
	}
}

// Exactly ONE half-open probe. If every suppressed caller probed at once, a
// recovering embedder would take the whole backlog the moment it came back —
// the outage the breaker exists to prevent.
func TestOnlyOneProbeIsGrantedWhenTheDelayElapses(t *testing.T) {
	resetBreaker(t)
	for i := 0; i < breakerThreshold; i++ {
		breaker.reportFailure(1000)
	}
	after := breaker.retryAtMS + 1

	if allowed, _ := breaker.allow(after); !allowed {
		t.Fatal("the first caller after the delay must get the probe")
	}
	if allowed, _ := breaker.allow(after); allowed {
		t.Fatal("a second concurrent caller must not also probe")
	}
}

// A backward wall-clock jump must not extend a bounded outage forever: if now
// precedes the moment it opened, the delay can never elapse on its own.
func TestBackwardClockJumpDoesNotStrandTheBreakerOpen(t *testing.T) {
	resetBreaker(t)
	for i := 0; i < breakerThreshold; i++ {
		breaker.reportFailure(1_000_000)
	}
	if allowed, _ := breaker.allow(1); !allowed {
		t.Fatal("a now earlier than opened_at must be eligible to probe")
	}
}

// A LOCAL refusal never blames the dependency: it releases the claimed probe so
// a misconfiguration cannot open the breaker against a service never contacted.
func TestLocalRefusalReleasesTheProbeWithoutCountingAFailure(t *testing.T) {
	resetBreaker(t)
	stub := &embedStub{reply: `[1]`}
	base := stub.start(t)

	out := Embed(EmbedRequest{BaseURL: base, Text: "", MaxDim: 4, NowMS: 1000})
	if out.Error == "" {
		t.Fatal("empty text must be refused")
	}
	if stub.calls != 0 {
		t.Fatal("nothing should have been sent")
	}
	if breaker.failureStreak != 0 {
		t.Fatalf("a local refusal is not a dependency failure: streak = %d", breaker.failureStreak)
	}
	if breaker.probeInflight {
		t.Fatal("the claimed probe must be released")
	}
}

func TestSuccessClosesAnEarlierOutage(t *testing.T) {
	resetBreaker(t)
	breaker.reportFailure(1000)
	breaker.reportFailure(1000)
	stub := &embedStub{reply: `[1,2]`}
	if out := Embed(EmbedRequest{BaseURL: stub.start(t), Text: "t", MaxDim: 4, NowMS: 2000}); out.Error != "" {
		t.Fatalf("unexpected: %+v", out)
	}
	if breaker.failureStreak != 0 || breaker.openCount != 0 {
		t.Fatalf("success must reset the streak: streak=%d openCount=%d",
			breaker.failureStreak, breaker.openCount)
	}
}

func TestDelayGrowsAndIsCapped(t *testing.T) {
	// Exponential from the base, capped at the max — and deterministic, so it
	// can be asserted at all.
	first := breakerDelayMS(0, 1000, breakerBaseMS, breakerMaxMS)
	second := breakerDelayMS(1, 1000, breakerBaseMS, breakerMaxMS)
	if first < breakerBaseMS || second <= first {
		t.Fatalf("delay must grow: %d then %d", first, second)
	}
	if capped := breakerDelayMS(99, 1000, breakerBaseMS, breakerMaxMS); capped > breakerMaxMS {
		t.Fatalf("delay %d exceeded the cap %d", capped, breakerMaxMS)
	}
	if again := breakerDelayMS(1, 1000, breakerBaseMS, breakerMaxMS); again != second {
		t.Fatal("the same inputs must give the same delay")
	}
}

func TestNonHTTPEmbedderIsDeclinedWithoutTouchingTheBreaker(t *testing.T) {
	resetBreaker(t)
	out := Embed(EmbedRequest{BaseURL: "/usr/local/bin/embed", Text: "t", MaxDim: 4, NowMS: 1000})
	if out.Error == "" {
		t.Fatal("a program-based embedder is not served here")
	}
	// That path's health is still accounted for in C; touching this breaker
	// would corrupt an accounting this module does not own.
	if breaker.failureStreak != 0 || breaker.probeInflight {
		t.Fatalf("breaker must be untouched: streak=%d probe=%v",
			breaker.failureStreak, breaker.probeInflight)
	}
	if !EmbedIsHTTP("https://host/x") || EmbedIsHTTP("/usr/bin/x") {
		t.Fatal("EmbedIsHTTP misclassified")
	}
}

func TestHandleEmbedRoutesThroughStageThree(t *testing.T) {
	resetBreaker(t)
	stub := &embedStub{reply: `[0.25,0.5]`}
	request, err := json.Marshal(EmbedRequest{
		BaseURL: stub.start(t), InputType: "document", Text: "t", MaxDim: 4, NowMS: 1000,
	})
	if err != nil {
		t.Fatal(err)
	}
	body, status := Handle(bus.ModuleInvocation{StageID: StageEmbed}, request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var decoded EmbedResponse
	if err := json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.Dim != 2 || decoded.Vector[1] != 0.5 {
		t.Fatalf("decoded = %+v", decoded)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageEmbed}, []byte("{")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed JSON must be rejected, got %v", status)
	}
}
